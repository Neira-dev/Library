#include "CAPShader.h"
#include "CapsuleAssetProtector.h"

#include <d3dcompiler.h>
#include <d3d12shader.h>
#include <algorithm>
#include <cctype>
#include <cstddef>
#include <filesystem>

#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "d3d12.lib")

namespace cap
{
	namespace
	{
		constexpr uint32_t kShaderVersion = 1;

		std::string ToLower(std::string text)
		{
			std::transform(text.begin(), text.end(), text.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
			return text;
		}

		bool StartsWith(const std::string& text, const char* prefix)
		{
			return text.rfind(prefix, 0) == 0;
		}

		bool EndsWith(const std::string& text, const char* suffix)
		{
			const size_t length = strlen(suffix);
			return text.size() >= length && text.compare(text.size() - length, length, suffix) == 0;
		}

		/**
		 * @brief ファイル名からシェーダーの種類を判定する
		 */
		bool DetectStage(const std::string& path, ShaderStage& outStage)
		{
			const std::string stem = ToLower(ToNarrow(std::filesystem::path(ToWide(path)).stem().wstring()));
			if (StartsWith(stem, "vs_") || StartsWith(stem, "vs.") || EndsWith(stem, "_vs") || EndsWith(stem, ".vs"))
			{
				outStage = ShaderStage::Vertex;
				return true;
			}
			if (StartsWith(stem, "ps_") || StartsWith(stem, "ps.") || EndsWith(stem, "_ps") || EndsWith(stem, ".ps"))
			{
				outStage = ShaderStage::Pixel;
				return true;
			}
			return false;
		}

		bool Compile(const std::string& path, const char* target, ComPtr<ID3DBlob>& outCode, std::string& outErrors)
		{
			UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
			// デバッグ時はデバッグ情報付き・最適化無し(グラフィックスデバッガで値を確認しやすくするため)
			flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
			flags |= D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif
			ComPtr<ID3DBlob> errors;
			const HRESULT hr = D3DCompileFromFile(ToWide(path).c_str(), nullptr, D3D_COMPILE_STANDARD_FILE_INCLUDE,
				"main", target, flags, 0, &outCode, &errors);
			if (FAILED(hr))
			{
				outErrors = errors
					? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize())
					: std::string("ファイルを開けません");
				return false;
			}
			return true;
		}

		// ---------------------------------------------
		// リフレクション
		// ---------------------------------------------
		enum class BindClass
		{
			ConstantBuffer,
			ShaderResource,
			Sampler,
			Unsupported,
		};

		BindClass Classify(D3D_SHADER_INPUT_TYPE type)
		{
			switch (type)
			{
			case D3D_SIT_CBUFFER:
				return BindClass::ConstantBuffer;
			case D3D_SIT_TBUFFER:
			case D3D_SIT_TEXTURE:
			case D3D_SIT_STRUCTURED:
			case D3D_SIT_BYTEADDRESS:
				return BindClass::ShaderResource;
			case D3D_SIT_SAMPLER:
				return BindClass::Sampler;
			default:
				return BindClass::Unsupported; // UAV等(グラフィックスパイプラインでは未対応)
			}
		}

		struct Binding
		{
			std::string name;
			BindClass bindClass = BindClass::ConstantBuffer;
			UINT shaderRegister = 0;
			UINT registerSpace = 0;
			D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;
		};

		bool CollectBindings(ID3D12ShaderReflection* reflection, D3D12_SHADER_VISIBILITY visibility, std::vector<Binding>& bindings)
		{
			D3D12_SHADER_DESC desc = {};
			reflection->GetDesc(&desc);

			for (UINT i = 0; i < desc.BoundResources; ++i)
			{
				D3D12_SHADER_INPUT_BIND_DESC bindDesc = {};
				reflection->GetResourceBindingDesc(i, &bindDesc);

				const BindClass bindClass = Classify(bindDesc.Type);
				if (bindClass == BindClass::Unsupported)
				{
					SetError(std::string("未対応のシェーダーリソースです(UAV等): ") + bindDesc.Name);
					return false;
				}
				if (bindClass == BindClass::ShaderResource && bindDesc.BindCount != 1)
				{
					SetError(std::string("テクスチャの配列は未対応です: ") + bindDesc.Name);
					return false;
				}

				// 頂点/ピクセルの両方で同じレジスタを使っていれば1つにまとめる
				auto it = std::find_if(bindings.begin(), bindings.end(), [&](const Binding& b) {
					return b.bindClass == bindClass && b.shaderRegister == bindDesc.BindPoint && b.registerSpace == bindDesc.Space;
					});
				if (it != bindings.end())
				{
					if (it->visibility != visibility)
					{
						it->visibility = D3D12_SHADER_VISIBILITY_ALL;
					}
					continue;
				}

				Binding binding;
				binding.name = bindDesc.Name;
				binding.bindClass = bindClass;
				binding.shaderRegister = bindDesc.BindPoint;
				binding.registerSpace = bindDesc.Space;
				binding.visibility = visibility;
				bindings.push_back(binding);
			}
			return true;
		}

		D3D12_STATIC_SAMPLER_DESC MakeSampler(const Binding& binding)
		{
			// サンプラーの設定は変数名から決める(名前に含まれる単語で切り替え)
			const std::string name = ToLower(binding.name);

			D3D12_STATIC_SAMPLER_DESC sampler = {};
			sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
			sampler.MaxAnisotropy = 1;
			if (name.find("point") != std::string::npos)
			{
				sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
			}
			else if (name.find("aniso") != std::string::npos)
			{
				sampler.Filter = D3D12_FILTER_ANISOTROPIC;
				sampler.MaxAnisotropy = 16;
			}

			D3D12_TEXTURE_ADDRESS_MODE address = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
			if (name.find("clamp") != std::string::npos)
			{
				address = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
			}
			else if (name.find("mirror") != std::string::npos)
			{
				address = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
			}
			sampler.AddressU = address;
			sampler.AddressV = address;
			sampler.AddressW = address;
			sampler.MipLODBias = 0.0f;
			sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
			sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
			sampler.MinLOD = 0.0f;
			sampler.MaxLOD = D3D12_FLOAT32_MAX;
			sampler.ShaderRegister = binding.shaderRegister;
			sampler.RegisterSpace = binding.registerSpace;
			sampler.ShaderVisibility = binding.visibility;
			return sampler;
		}

		// ---------------------------------------------
		// 頂点レイアウト
		// ---------------------------------------------
		struct StandardElement
		{
			const char* semantic;
			DXGI_FORMAT format;
			UINT offset;
		};

		// CAPの標準頂点(CapsuleAssetProtector::Vertex)の各要素
		const StandardElement kStandardElements[] = {
			{ "POSITION", DXGI_FORMAT_R32G32B32_FLOAT,    static_cast<UINT>(offsetof(CapsuleAssetProtector::Vertex, position)) },
			{ "NORMAL",   DXGI_FORMAT_R32G32B32_FLOAT,    static_cast<UINT>(offsetof(CapsuleAssetProtector::Vertex, normal)) },
			{ "TEXCOORD", DXGI_FORMAT_R32G32_FLOAT,       static_cast<UINT>(offsetof(CapsuleAssetProtector::Vertex, uv)) },
			{ "TANGENT",  DXGI_FORMAT_R32G32B32A32_FLOAT, static_cast<UINT>(offsetof(CapsuleAssetProtector::Vertex, tangent)) },
			{ "COLOR",    DXGI_FORMAT_R32G32B32A32_FLOAT, static_cast<UINT>(offsetof(CapsuleAssetProtector::Vertex, color)) },
		};

		bool BuildInputLayout(ID3D12ShaderReflection* vertexReflection, std::vector<D3D12_INPUT_ELEMENT_DESC>& outElements)
		{
			D3D12_SHADER_DESC desc = {};
			vertexReflection->GetDesc(&desc);

			for (UINT i = 0; i < desc.InputParameters; ++i)
			{
				D3D12_SIGNATURE_PARAMETER_DESC param = {};
				vertexReflection->GetInputParameterDesc(i, &param);

				// SV_VertexID等のシステム値は頂点バッファから来ないので除外する
				if (param.SystemValueType != D3D_NAME_UNDEFINED)
				{
					continue;
				}

				const StandardElement* found = nullptr;
				if (param.SemanticIndex == 0)
				{
					for (const StandardElement& element : kStandardElements)
					{
						if (_stricmp(element.semantic, param.SemanticName) == 0)
						{
							found = &element;
							break;
						}
					}
				}
				if (found == nullptr)
				{
					SetError(std::string("頂点シェーダーの入力 ") + param.SemanticName + std::to_string(param.SemanticIndex) +
						" はCAPの標準頂点にありません(使えるのは POSITION / NORMAL / TEXCOORD / TANGENT / COLOR)");
					return false;
				}

				D3D12_INPUT_ELEMENT_DESC element = {};
				element.SemanticName = found->semantic; // 静的な文字列を使う(リフレクションの寿命に依存しない)
				element.SemanticIndex = 0;
				element.Format = found->format;
				element.InputSlot = 0;
				element.AlignedByteOffset = found->offset;
				element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
				element.InstanceDataStepRate = 0;
				outElements.push_back(element);
			}
			return true;
		}

		/**
		 * @brief テクスチャスロットの役割を変数名から決める(メッシュ描画時の自動割り当て用)
		 */
		void AssignTextureRoles(std::vector<PipelineSlot>& slots)
		{
			static const char* const kDiffuseWords[] = { "diffuse", "albedo", "base", "color", "main" };

			PipelineSlot* firstOther = nullptr;
			bool hasDiffuse = false;
			for (PipelineSlot& slot : slots)
			{
				if (slot.kind != SlotKind::Texture)
				{
					continue;
				}
				const std::string name = ToLower(slot.name);
				if (name.find("normal") != std::string::npos)
				{
					slot.role = TextureRole::Normal;
					continue;
				}
				if (!hasDiffuse)
				{
					for (const char* word : kDiffuseWords)
					{
						if (name.find(word) != std::string::npos)
						{
							slot.role = TextureRole::Diffuse;
							hasDiffuse = true;
							break;
						}
					}
				}
				if (slot.role == TextureRole::Other && firstOther == nullptr)
				{
					firstOther = &slot;
				}
			}

			// それらしい名前が無ければ、最初のテクスチャをベースカラーとして扱う
			if (!hasDiffuse && firstOther != nullptr)
			{
				firstOther->role = TextureRole::Diffuse;
			}
		}
	}

	bool CookShader(const std::string& path, std::vector<uint8_t>& outCooked)
	{
		ShaderStage stage = ShaderStage::Vertex;
		ComPtr<ID3DBlob> code;
		std::string errors;

		if (DetectStage(path, stage))
		{
			const char* target = (stage == ShaderStage::Vertex) ? "vs_5_0" : "ps_5_0";
			if (!Compile(path, target, code, errors))
			{
				SetError("シェーダーのコンパイルに失敗しました: " + path + "\n" + errors);
				return false;
			}
		}
		else
		{
			// 名前から判定できない場合は、頂点→ピクセルの順に試す
			std::string vertexErrors;
			std::string pixelErrors;
			if (Compile(path, "vs_5_0", code, vertexErrors))
			{
				stage = ShaderStage::Vertex;
			}
			else if (Compile(path, "ps_5_0", code, pixelErrors))
			{
				stage = ShaderStage::Pixel;
			}
			else
			{
				SetError("シェーダーのコンパイルに失敗しました: " + path +
					"\n[頂点シェーダーとして]\n" + vertexErrors + "\n[ピクセルシェーダーとして]\n" + pixelErrors);
				return false;
			}
		}

		BinaryWriter writer;
		writer.Write<uint32_t>(kShaderVersion);
		writer.Write<uint32_t>(static_cast<uint32_t>(stage));
		writer.Write<uint32_t>(static_cast<uint32_t>(code->GetBufferSize()));
		writer.WriteBytes(code->GetBufferPointer(), code->GetBufferSize());
		outCooked = std::move(writer.Buffer());
		return true;
	}

	std::unique_ptr<ShaderAsset> CreateShaderAsset(const std::vector<uint8_t>& cooked)
	{
		BinaryReader reader(cooked);
		uint32_t version = 0;
		uint32_t stage = 0;
		uint32_t size = 0;
		if (!reader.Read(version) || version != kShaderVersion || !reader.Read(stage) || !reader.Read(size) || size == 0)
		{
			SetError("シェーダーのクック済みデータが壊れています");
			return nullptr;
		}
		const uint8_t* code = reader.Take(size);
		if (code == nullptr)
		{
			SetError("シェーダーのクック済みデータが壊れています");
			return nullptr;
		}

		auto asset = std::make_unique<ShaderAsset>();
		asset->type = AssetType::Shader;
		asset->stage = static_cast<ShaderStage>(stage);
		asset->bytecode.assign(code, code + size);
		asset->memoryBytes = size;
		return asset;
	}

	bool BuildPipeline(ID3D12Device* device, const ShaderAsset& vertexShader, const ShaderAsset& pixelShader,
		const PipelineOptions& options, Pipeline& outPipeline)
	{
		ComPtr<ID3D12ShaderReflection> vertexReflection;
		ComPtr<ID3D12ShaderReflection> pixelReflection;
		if (FAILED(D3DReflect(vertexShader.bytecode.data(), vertexShader.bytecode.size(), IID_PPV_ARGS(&vertexReflection))) ||
			FAILED(D3DReflect(pixelShader.bytecode.data(), pixelShader.bytecode.size(), IID_PPV_ARGS(&pixelReflection))))
		{
			SetError("シェーダーのリフレクションに失敗しました");
			return false;
		}

		// ---------------------------------------------
		// ルートシグネチャ
		// ---------------------------------------------
		std::vector<Binding> bindings;
		if (!CollectBindings(vertexReflection.Get(), D3D12_SHADER_VISIBILITY_VERTEX, bindings) ||
			!CollectBindings(pixelReflection.Get(), D3D12_SHADER_VISIBILITY_PIXEL, bindings))
		{
			return false;
		}

		std::vector<D3D12_ROOT_PARAMETER> rootParams;
		std::vector<D3D12_DESCRIPTOR_RANGE> ranges;
		std::vector<D3D12_STATIC_SAMPLER_DESC> samplers;
		std::vector<PipelineSlot> slots;
		// ディスクリプタテーブルはrangesの要素を指すので、途中で再確保されないよう先に確保しておく
		ranges.reserve(bindings.size());
		rootParams.reserve(bindings.size());

		for (const Binding& binding : bindings)
		{
			if (binding.bindClass == BindClass::Sampler)
			{
				samplers.push_back(MakeSampler(binding));
				continue;
			}

			D3D12_ROOT_PARAMETER param = {};
			param.ShaderVisibility = binding.visibility;

			PipelineSlot slot;
			slot.name = binding.name;
			slot.rootIndex = static_cast<uint32_t>(rootParams.size());

			if (binding.bindClass == BindClass::ConstantBuffer)
			{
				param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
				param.Descriptor.ShaderRegister = binding.shaderRegister;
				param.Descriptor.RegisterSpace = binding.registerSpace;
				slot.kind = SlotKind::ConstantBuffer;
			}
			else
			{
				D3D12_DESCRIPTOR_RANGE range = {};
				range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
				range.NumDescriptors = 1;
				range.BaseShaderRegister = binding.shaderRegister;
				range.RegisterSpace = binding.registerSpace;
				range.OffsetInDescriptorsFromTableStart = 0;
				ranges.push_back(range);

				param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
				param.DescriptorTable.NumDescriptorRanges = 1;
				param.DescriptorTable.pDescriptorRanges = &ranges.back();
				slot.kind = SlotKind::Texture;
			}

			rootParams.push_back(param);
			slots.push_back(slot);
		}
		AssignTextureRoles(slots);

		D3D12_ROOT_SIGNATURE_DESC rootSignatureDesc = {};
		rootSignatureDesc.NumParameters = static_cast<UINT>(rootParams.size());
		rootSignatureDesc.pParameters = rootParams.empty() ? nullptr : rootParams.data();
		rootSignatureDesc.NumStaticSamplers = static_cast<UINT>(samplers.size());
		rootSignatureDesc.pStaticSamplers = samplers.empty() ? nullptr : samplers.data();
		rootSignatureDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

		ComPtr<ID3DBlob> signatureBlob;
		ComPtr<ID3DBlob> errorBlob;
		if (FAILED(D3D12SerializeRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1, &signatureBlob, &errorBlob)))
		{
			std::string message = "ルートシグネチャの作成に失敗しました";
			if (errorBlob)
			{
				message += "\n" + std::string(static_cast<const char*>(errorBlob->GetBufferPointer()), errorBlob->GetBufferSize());
			}
			SetError(message);
			return false;
		}

		Pipeline pipeline;
		if (FAILED(device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
			IID_PPV_ARGS(&pipeline.rootSignature))))
		{
			SetError("ルートシグネチャの作成に失敗しました");
			return false;
		}

		// ---------------------------------------------
		// 頂点レイアウト
		// ---------------------------------------------
		std::vector<D3D12_INPUT_ELEMENT_DESC> inputElements;
		if (!BuildInputLayout(vertexReflection.Get(), inputElements))
		{
			return false;
		}

		// ---------------------------------------------
		// PSO
		// ---------------------------------------------
		D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
		psoDesc.pRootSignature = pipeline.rootSignature.Get();
		psoDesc.VS = { vertexShader.bytecode.data(), vertexShader.bytecode.size() };
		psoDesc.PS = { pixelShader.bytecode.data(), pixelShader.bytecode.size() };
		psoDesc.InputLayout = { inputElements.empty() ? nullptr : inputElements.data(), static_cast<UINT>(inputElements.size()) };
		psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;

		// ラスタライザ
		psoDesc.RasterizerState.FillMode = options.wireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
		psoDesc.RasterizerState.CullMode = options.cullMode;
		psoDesc.RasterizerState.FrontCounterClockwise = FALSE;
		psoDesc.RasterizerState.DepthBias = D3D12_DEFAULT_DEPTH_BIAS;
		psoDesc.RasterizerState.DepthBiasClamp = D3D12_DEFAULT_DEPTH_BIAS_CLAMP;
		psoDesc.RasterizerState.SlopeScaledDepthBias = D3D12_DEFAULT_SLOPE_SCALED_DEPTH_BIAS;
		psoDesc.RasterizerState.DepthClipEnable = TRUE;
		psoDesc.RasterizerState.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;

		// ブレンド
		D3D12_RENDER_TARGET_BLEND_DESC& blend = psoDesc.BlendState.RenderTarget[0];
		blend.BlendEnable = options.alphaBlend ? TRUE : FALSE;
		blend.LogicOpEnable = FALSE;
		blend.SrcBlend = D3D12_BLEND_SRC_ALPHA;
		blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
		blend.BlendOp = D3D12_BLEND_OP_ADD;
		blend.SrcBlendAlpha = D3D12_BLEND_ONE;
		blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
		blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
		blend.LogicOp = D3D12_LOGIC_OP_NOOP;
		blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

		// 深度・ステンシル
		const bool hasDepth = options.dsvFormat != DXGI_FORMAT_UNKNOWN;
		psoDesc.DepthStencilState.DepthEnable = (hasDepth && options.depthTest) ? TRUE : FALSE;
		psoDesc.DepthStencilState.DepthWriteMask = (hasDepth && options.depthWrite) ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
		psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
		psoDesc.DepthStencilState.StencilEnable = FALSE;
		psoDesc.DepthStencilState.StencilReadMask = D3D12_DEFAULT_STENCIL_READ_MASK;
		psoDesc.DepthStencilState.StencilWriteMask = D3D12_DEFAULT_STENCIL_WRITE_MASK;
		const D3D12_DEPTH_STENCILOP_DESC stencilOp = { D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_KEEP, D3D12_COMPARISON_FUNC_ALWAYS };
		psoDesc.DepthStencilState.FrontFace = stencilOp;
		psoDesc.DepthStencilState.BackFace = stencilOp;
		psoDesc.DSVFormat = options.dsvFormat;

		psoDesc.SampleMask = UINT_MAX;
		psoDesc.SampleDesc.Count = 1;
		psoDesc.NumRenderTargets = 1;
		psoDesc.RTVFormats[0] = options.rtvFormat;

		if (FAILED(device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&pipeline.pipelineState))))
		{
			SetError("PSOの作成に失敗しました(シェーダーの入出力やフォーマットを確認してください)");
			return false;
		}

		pipeline.slots = std::move(slots);
		outPipeline = std::move(pipeline);
		return true;
	}
}
