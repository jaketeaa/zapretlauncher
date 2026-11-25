#pragma once
#include <d3d11.h>
#include <wrl/client.h>
#include "imgui.h"

class GlassRenderer {
public:
    bool Prepare(ID3D11Device* device, ID3D11DeviceContext* immediate, int width, int height);
    ID3D11ShaderResourceView* Backdrop() const { return background_.sample.Get(); }
    bool DrawGlass(ImDrawList* list, ImVec2 a, ImVec2 b, float radius, float dpi,
                   ImU32 tint, float hover, float warmth, float glow = 0) const;

private:
    struct Surface {
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> target;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> sample;
        bool Create(ID3D11Device* device, UINT width, UINT height);
    };

    bool Initialize(ID3D11Device* device);
    static void BindGlass(const ImDrawList*, const ImDrawCmd* command);
    static void RestoreGlass(const ImDrawList*, const ImDrawCmd*);

    Microsoft::WRL::ComPtr<ID3D11DeviceContext> recording_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vertex_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> backdrop_, horizontal_, vertical_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> settings_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> lens_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> lensSettings_;
    Surface background_, intermediate_, frosted_;
    int width_ = 0;
    int height_ = 0;
    bool ready_ = false;
};
