#include "glass_renderer.h"
#include <d3dcompiler.h>
#include <algorithm>
#include "imgui_impl_dx11.h"

using Microsoft::WRL::ComPtr;

namespace {
constexpr char kShader[] = R"hlsl(
cbuffer Settings : register(b0) { float2 texel; float2 padding; };
Texture2D sourceLayer : register(t0);
SamplerState linearClamp : register(s0);
struct Varying { float4 pos : SV_Position; float2 uv : TEXCOORD0; };

Varying Vertex(uint id : SV_VertexID) {
    Varying v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.pos = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
float light(float2 uv, float2 center, float2 scale) {
    float2 d = (uv - center) / scale;
    return exp(-dot(d, d));
}
float4 Background(Varying v) : SV_Target {
    float3 graphite = float3(12, 12, 14) / 255.0;
    float sheen = light(v.uv, float2(0.38, -0.06), float2(0.46, 0.33));
    float graphiteGlow = light(v.uv, float2(0.82, 0.39), float2(0.29, 0.29));
    return float4(graphite + sheen * float3(0.032, 0.030, 0.027)
                         + graphiteGlow * float3(0.015, 0.014, 0.011), 1);
}
float4 Blur(float2 uv, float2 step) {
    float4 color = sourceLayer.Sample(linearClamp, uv) * 0.227027;
    color += (sourceLayer.Sample(linearClamp, uv + step * 1.384615) +
              sourceLayer.Sample(linearClamp, uv - step * 1.384615)) * 0.316216;
    color += (sourceLayer.Sample(linearClamp, uv + step * 3.230769) +
              sourceLayer.Sample(linearClamp, uv - step * 3.230769)) * 0.070270;
    return color;
}
float4 Horizontal(Varying v) : SV_Target { return Blur(v.uv, float2(texel.x, 0)); }
float4 Vertical(Varying v) : SV_Target { return Blur(v.uv, float2(0, texel.y)); }
)hlsl";

constexpr char kGlassShader[] = R"hlsl(
cbuffer Lens : register(b7) {
    float2 size;
    float radius;
    float dpi;
    float2 origin;
    float2 viewport;
    float hover;
    float warmth;
    float glow;
    float padding;
};
Texture2D background : register(t0);
SamplerState linearClamp : register(s0);
struct Input {
    float4 position : SV_POSITION;
    float4 tint : COLOR0;
    float2 uv : TEXCOORD0;
};
float signedDistance(float2 p, float2 halfSize, float r) {
    float2 corner = abs(p) - halfSize + r;
    return length(max(corner, 0)) + min(max(corner.x, corner.y), 0) - r;
}
float4 Pixel(Input input) : SV_Target {
    float2 local = (input.uv - 0.5) * size;
    float d = signedDistance(local, size * 0.5, radius);
    float aa = max(fwidth(d) * 0.52, 0.55);
    float coverage = 1 - smoothstep(-aa, aa, d);
    float shadowDist = signedDistance(local - float2(0, 3.0 * dpi), size * 0.5, radius);
    float shadow = 0.22 * exp(-pow(max(shadowDist, 0) / (6.0 * dpi), 2.0));
    float halo = glow * exp(-pow(max(d, 0) / (16.0 * dpi), 2.0));
    float2 blurredUV = (origin + (0.5 + (input.uv - 0.5) * 0.9) * size) / viewport;
    float3 blurred = background.Sample(linearClamp, blurredUV).rgb;
    float3 surface = lerp(input.tint.rgb, blurred, lerp(0.22, 0.05, warmth));
    float top = saturate(1 - input.uv.y);
    float light = (0.075 + 0.035 * hover) * pow(top, 3);
    float rim = exp(-pow((d + 1.25 * dpi) / (1.10 * dpi), 2.0));
    light += rim * (0.024 + 0.09 * top + 0.035 * hover);
    surface = lerp(surface, float3(1.0, 0.98, 0.94), saturate(light));
    const float3 amber = float3(1, 0.65, 0.095);
    float outer = 1 - (1 - shadow) * (1 - halo);
    float3 outside = amber * halo / max(outer, 0.0001);
    float alpha = coverage + outer * (1 - coverage);
    // The ImGui DX11 backend blends straight alpha.
    float3 color = (surface * coverage + outside * outer * (1 - coverage)) / max(alpha, 0.0001);
    return float4(color, alpha * input.tint.a);
}
)hlsl";

struct LensParams {
    float width, height, radius, dpi;
    float x, y, viewportWidth, viewportHeight;
    float hover, warmth, glow, padding;
};

thread_local ComPtr<ID3D11Buffer> previousLensBuffer;

struct LensCommand {
    ID3D11PixelShader* shader;
    ID3D11Buffer* buffer;
    LensParams params;
};

HRESULT Compile(const char* entry, const char* profile, ID3DBlob** result) {
    return D3DCompile(kShader, sizeof(kShader) - 1, "GlassRenderer", nullptr, nullptr, entry, profile,
                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, result, nullptr);
}
}

bool GlassRenderer::Surface::Create(ID3D11Device* device, UINT width, UINT height) {
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)) &&
           SUCCEEDED(device->CreateRenderTargetView(texture.Get(), nullptr, &target)) &&
           SUCCEEDED(device->CreateShaderResourceView(texture.Get(), nullptr, &sample));
}

bool GlassRenderer::Initialize(ID3D11Device* device) {
    ComPtr<ID3DBlob> shader;
    if (FAILED(device->CreateDeferredContext(0, &recording_)) ||
        FAILED(Compile("Vertex", "vs_4_0", &shader)) ||
        FAILED(device->CreateVertexShader(shader->GetBufferPointer(), shader->GetBufferSize(), nullptr, &vertex_)))
        return false;

    auto pixel = [&](const char* entry, ID3D11PixelShader** output) {
        shader.Reset();
        return SUCCEEDED(Compile(entry, "ps_4_0", &shader)) &&
               SUCCEEDED(device->CreatePixelShader(shader->GetBufferPointer(), shader->GetBufferSize(), nullptr, output));
    };
    if (!pixel("Background", &backdrop_) || !pixel("Horizontal", &horizontal_) || !pixel("Vertical", &vertical_))
        return false;

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    D3D11_BUFFER_DESC buffer{};
    buffer.ByteWidth = 16;
    buffer.Usage = D3D11_USAGE_DEFAULT;
    buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateSamplerState(&sampler, &sampler_)) ||
        FAILED(device->CreateBuffer(&buffer, nullptr, &settings_))) return false;

    shader.Reset();
    buffer.ByteWidth = sizeof(LensParams);
    // The background and blur remain available if analytic glass is unsupported.
    if (SUCCEEDED(D3DCompile(kGlassShader, sizeof(kGlassShader) - 1, "GlassSurface", nullptr, nullptr,
                            "Pixel", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, nullptr)) &&
        SUCCEEDED(device->CreatePixelShader(shader->GetBufferPointer(), shader->GetBufferSize(), nullptr, &lens_))) {
        if (FAILED(device->CreateBuffer(&buffer, nullptr, &lensSettings_))) lens_.Reset();
    }
    return true;
}

void GlassRenderer::BindGlass(const ImDrawList*, const ImDrawCmd* command) {
    const auto& lens = *static_cast<const LensCommand*>(command->UserCallbackData);
    auto* state = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    state->DeviceContext->UpdateSubresource(lens.buffer, 0, nullptr, &lens.params, 0, 0);
    state->DeviceContext->PSGetConstantBuffers(7, 1, &previousLensBuffer);
    state->DeviceContext->PSSetConstantBuffers(7, 1, &lens.buffer);
    state->DeviceContext->PSSetShader(lens.shader, nullptr, 0);
}

void GlassRenderer::RestoreGlass(const ImDrawList*, const ImDrawCmd*) {
    auto* state = static_cast<ImGui_ImplDX11_RenderState*>(ImGui::GetPlatformIO().Renderer_RenderState);
    state->DeviceContext->PSSetConstantBuffers(7, 1, previousLensBuffer.GetAddressOf());
    previousLensBuffer.Reset();
}

bool GlassRenderer::DrawGlass(ImDrawList* list, ImVec2 a, ImVec2 b, float radius, float dpi,
                              ImU32 tint, float hover, float warmth, float glow) const {
    if (!ready_ || !lens_ || !lensSettings_) return false;
    const auto* viewport = ImGui::GetMainViewport();
    const float w = b.x - a.x, h = b.y - a.y;
    if (w <= 0 || h <= 0 || (tint >> 24) == 0) return true;
    LensCommand command{lens_.Get(), lensSettings_.Get(),
        {w, h, std::min(radius, std::min(w, h) / 2.0f), dpi,
         a.x - viewport->Pos.x, a.y - viewport->Pos.y, viewport->Size.x, viewport->Size.y,
         hover, warmth, glow, 0}};
    const float pad = (glow > 0 ? 39.0f : 16.0f) * dpi;
    list->AddCallback(BindGlass, &command, sizeof(command));
    list->AddImage(reinterpret_cast<ImTextureID>(frosted_.sample.Get()),
                   {a.x - pad, a.y - pad}, {b.x + pad, b.y + pad},
                   {-pad / w, -pad / h}, {1 + pad / w, 1 + pad / h}, tint);
    list->AddCallback(RestoreGlass, nullptr);
    list->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
    return true;
}

bool GlassRenderer::Prepare(ID3D11Device* device, ID3D11DeviceContext* immediate, int width, int height) {
    if (width == width_ && height == height_) return ready_;
    width_ = width;
    height_ = height;
    ready_ = false;
    if (!settings_ && !Initialize(device)) return false;

    const UINT w = static_cast<UINT>(std::max(width / 4, 1));
    const UINT h = static_cast<UINT>(std::max(height / 4, 1));
    Surface source, intermediate, blurred;
    if (!source.Create(device, w, h) || !intermediate.Create(device, w, h) || !blurred.Create(device, w, h))
        return false;

    const D3D11_VIEWPORT viewport{0, 0, static_cast<float>(w), static_cast<float>(h), 0, 1};
    const float params[]{1.55f / w, 1.55f / h, 0, 0};
    auto* ctx = recording_.Get();
    ctx->ClearState();
    ctx->RSSetViewports(1, &viewport);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(vertex_.Get(), nullptr, 0);
    ctx->UpdateSubresource(settings_.Get(), 0, nullptr, params, 0, 0);
    ctx->PSSetConstantBuffers(0, 1, settings_.GetAddressOf());
    ctx->PSSetSamplers(0, 1, sampler_.GetAddressOf());

    auto pass = [&](Surface& output, ID3D11PixelShader* shader, ID3D11ShaderResourceView* input) {
        ctx->OMSetRenderTargets(1, output.target.GetAddressOf(), nullptr);
        ctx->PSSetShaderResources(0, 1, &input);
        ctx->PSSetShader(shader, nullptr, 0);
        ctx->Draw(3, 0);
        input = nullptr;
        ctx->PSSetShaderResources(0, 1, &input);
    };
    pass(source, backdrop_.Get(), nullptr);
    pass(intermediate, horizontal_.Get(), source.sample.Get());
    pass(blurred, vertical_.Get(), intermediate.sample.Get());

    ComPtr<ID3D11CommandList> commands;
    if (FAILED(ctx->FinishCommandList(FALSE, &commands))) return false;
    immediate->ExecuteCommandList(commands.Get(), TRUE);
    background_ = std::move(source);
    intermediate_ = std::move(intermediate);
    frosted_ = std::move(blurred);
    return ready_ = true;
}
