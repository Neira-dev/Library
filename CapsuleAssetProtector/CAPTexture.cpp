#include "CAPTexture.h"

#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <cstring>

#pragma comment(lib, "windowscodecs.lib")

namespace cap
{
	namespace
	{
		constexpr uint32_t kTextureVersion = 1;

		/**
		 * @brief この処理の間だけCOMを使えるようにする
		 * @details 呼び出し元スレッドのCOM設定(STA/MTA)を壊さないよう、自分で初期化できた場合だけ後始末する
		 */
		class ComScope
		{
		public:
			ComScope()
			{
				const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
				// S_OK/S_FALSEは対になるCoUninitializeが必要。RPC_E_CHANGED_MODEは既に別モードで初期化済み(そのまま使える)
				_needUninitialize = SUCCEEDED(hr);
			}
			~ComScope()
			{
				if (_needUninitialize)
				{
					CoUninitialize();
				}
			}
		private:
			bool _needUninitialize = false;
		};

		bool DecodeFrame(IWICImagingFactory* factory, IWICBitmapDecoder* decoder, uint32_t& outWidth, uint32_t& outHeight, std::vector<uint8_t>& outRgba)
		{
			Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
			if (FAILED(decoder->GetFrame(0, &frame)))
			{
				return false;
			}

			// どんな形式の画像もRGBA 8bitに変換して扱う
			Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
			if (FAILED(factory->CreateFormatConverter(&converter)) ||
				FAILED(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA,
					WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom)))
			{
				return false;
			}

			UINT width = 0;
			UINT height = 0;
			if (FAILED(converter->GetSize(&width, &height)) || width == 0 || height == 0)
			{
				return false;
			}

			outRgba.resize(static_cast<size_t>(width) * height * 4);
			if (FAILED(converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(outRgba.size()), outRgba.data())))
			{
				return false;
			}
			outWidth = width;
			outHeight = height;
			return true;
		}

		/**
		 * @brief 1段小さいミップを作る(2x2ピクセルの平均。奇数サイズの端は最後の行/列を繰り返す)
		 */
		void Downsample(const uint8_t* src, uint32_t srcWidth, uint32_t srcHeight, std::vector<uint8_t>& dst, uint32_t& dstWidth, uint32_t& dstHeight)
		{
			dstWidth = (std::max)(1u, srcWidth / 2);
			dstHeight = (std::max)(1u, srcHeight / 2);
			dst.resize(static_cast<size_t>(dstWidth) * dstHeight * 4);

			for (uint32_t y = 0; y < dstHeight; ++y)
			{
				const uint32_t y0 = (std::min)(y * 2, srcHeight - 1);
				const uint32_t y1 = (std::min)(y * 2 + 1, srcHeight - 1);
				for (uint32_t x = 0; x < dstWidth; ++x)
				{
					const uint32_t x0 = (std::min)(x * 2, srcWidth - 1);
					const uint32_t x1 = (std::min)(x * 2 + 1, srcWidth - 1);
					const uint8_t* p00 = src + (static_cast<size_t>(y0) * srcWidth + x0) * 4;
					const uint8_t* p01 = src + (static_cast<size_t>(y0) * srcWidth + x1) * 4;
					const uint8_t* p10 = src + (static_cast<size_t>(y1) * srcWidth + x0) * 4;
					const uint8_t* p11 = src + (static_cast<size_t>(y1) * srcWidth + x1) * 4;
					uint8_t* out = dst.data() + (static_cast<size_t>(y) * dstWidth + x) * 4;
					for (int c = 0; c < 4; ++c)
					{
						out[c] = static_cast<uint8_t>((p00[c] + p01[c] + p10[c] + p11[c] + 2) / 4);
					}
				}
			}
		}
	}

	bool CookTextureFromRgba(uint32_t width, uint32_t height, const uint8_t* rgba, std::vector<uint8_t>& outCooked)
	{
		if (width == 0 || height == 0 || rgba == nullptr)
		{
			SetError("テクスチャのピクセルデータが不正です");
			return false;
		}

		// ミップレベル数 = 1x1になるまで半分にした回数 + 1
		uint32_t mipCount = 1;
		for (uint32_t size = (std::max)(width, height); size > 1; size /= 2)
		{
			++mipCount;
		}

		BinaryWriter writer;
		writer.Write<uint32_t>(kTextureVersion);
		writer.Write<uint32_t>(static_cast<uint32_t>(DXGI_FORMAT_R8G8B8A8_UNORM));
		writer.Write<uint32_t>(width);
		writer.Write<uint32_t>(height);
		writer.Write<uint32_t>(mipCount);

		std::vector<uint8_t> current(rgba, rgba + static_cast<size_t>(width) * height * 4);
		uint32_t mipWidth = width;
		uint32_t mipHeight = height;
		for (uint32_t mip = 0; mip < mipCount; ++mip)
		{
			const uint32_t rowPitch = mipWidth * 4;
			writer.Write<uint32_t>(mipWidth);
			writer.Write<uint32_t>(mipHeight);
			writer.Write<uint32_t>(rowPitch);
			writer.WriteBytes(current.data(), current.size());

			if (mip + 1 < mipCount)
			{
				std::vector<uint8_t> next;
				uint32_t nextWidth = 0;
				uint32_t nextHeight = 0;
				Downsample(current.data(), mipWidth, mipHeight, next, nextWidth, nextHeight);
				current = std::move(next);
				mipWidth = nextWidth;
				mipHeight = nextHeight;
			}
		}

		outCooked = std::move(writer.Buffer());
		return true;
	}

	bool CookTextureFromFile(const std::string& path, std::vector<uint8_t>& outCooked)
	{
		ComScope com;
		Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
		{
			SetError("WICの初期化に失敗しました");
			return false;
		}

		Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateDecoderFromFilename(ToWide(path).c_str(), nullptr, GENERIC_READ,
			WICDecodeMetadataCacheOnDemand, &decoder)))
		{
			SetError("画像を開けません(未対応の形式か、ファイルがありません): " + path);
			return false;
		}

		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> rgba;
		if (!DecodeFrame(factory.Get(), decoder.Get(), width, height, rgba))
		{
			SetError("画像のデコードに失敗しました: " + path);
			return false;
		}
		return CookTextureFromRgba(width, height, rgba.data(), outCooked);
	}

	bool CookTextureFromMemory(const void* data, size_t size, std::vector<uint8_t>& outCooked)
	{
		if (data == nullptr || size == 0)
		{
			SetError("埋め込みテクスチャのデータが空です");
			return false;
		}

		ComScope com;
		Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
		if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))))
		{
			SetError("WICの初期化に失敗しました");
			return false;
		}

		Microsoft::WRL::ComPtr<IWICStream> stream;
		Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
		if (FAILED(factory->CreateStream(&stream)) ||
			FAILED(stream->InitializeFromMemory(static_cast<BYTE*>(const_cast<void*>(data)), static_cast<DWORD>(size))) ||
			FAILED(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
		{
			SetError("埋め込みテクスチャを開けません(未対応の形式です)");
			return false;
		}

		uint32_t width = 0;
		uint32_t height = 0;
		std::vector<uint8_t> rgba;
		if (!DecodeFrame(factory.Get(), decoder.Get(), width, height, rgba))
		{
			SetError("埋め込みテクスチャのデコードに失敗しました");
			return false;
		}
		return CookTextureFromRgba(width, height, rgba.data(), outCooked);
	}

	std::unique_ptr<TextureAsset> CreateTextureAsset(CAPGpu& gpu, const std::vector<uint8_t>& cooked)
	{
		BinaryReader reader(cooked);
		uint32_t version = 0;
		uint32_t format = 0;
		uint32_t width = 0;
		uint32_t height = 0;
		uint32_t mipCount = 0;
		if (!reader.Read(version) || version != kTextureVersion ||
			!reader.Read(format) || !reader.Read(width) || !reader.Read(height) || !reader.Read(mipCount) ||
			mipCount == 0 || mipCount > 16)
		{
			SetError("テクスチャのクック済みデータが壊れています");
			return nullptr;
		}

		// クック済みデータの中を直接指す(コピーしない)
		std::vector<TextureMipData> mips(mipCount);
		uint64_t totalBytes = 0;
		for (uint32_t i = 0; i < mipCount; ++i)
		{
			uint32_t mipWidth = 0;
			uint32_t mipHeight = 0;
			uint32_t rowPitch = 0;
			if (!reader.Read(mipWidth) || !reader.Read(mipHeight) || !reader.Read(rowPitch))
			{
				SetError("テクスチャのクック済みデータが壊れています");
				return nullptr;
			}
			const size_t bytes = static_cast<size_t>(rowPitch) * mipHeight;
			const uint8_t* pixels = reader.Take(bytes);
			if (pixels == nullptr)
			{
				SetError("テクスチャのクック済みデータが壊れています");
				return nullptr;
			}
			mips[i].pixels = pixels;
			mips[i].rowPitch = rowPitch;
			mips[i].rows = mipHeight;
			totalBytes += bytes;
		}

		auto asset = std::make_unique<TextureAsset>();
		asset->type = AssetType::Texture;
		asset->width = width;
		asset->height = height;
		asset->memoryBytes = totalBytes;

		const DXGI_FORMAT dxgiFormat = static_cast<DXGI_FORMAT>(format);
		if (!gpu.CreateTexture2D(width, height, dxgiFormat, mips, asset->texture))
		{
			return nullptr;
		}

		asset->srvIndex = gpu.AllocateSrv();
		if (asset->srvIndex == UINT32_MAX)
		{
			return nullptr;
		}

		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
		srvDesc.Format = dxgiFormat;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		srvDesc.Texture2D.MipLevels = mipCount;
		gpu.Device()->CreateShaderResourceView(asset->texture.Get(), &srvDesc, gpu.CpuHandle(asset->srvIndex));
		return asset;
	}

	std::unique_ptr<TextureAsset> CreateSolidTexture(CAPGpu& gpu, uint8_t r, uint8_t g, uint8_t b, uint8_t a)
	{
		const uint8_t pixel[4] = { r, g, b, a };
		std::vector<uint8_t> cooked;
		if (!CookTextureFromRgba(1, 1, pixel, cooked))
		{
			return nullptr;
		}
		return CreateTextureAsset(gpu, cooked);
	}

	const std::vector<std::string>& TextureExtensions()
	{
		static const std::vector<std::string> extensions = {
			".png", ".jpg", ".jpeg", ".bmp", ".tif", ".tiff", ".gif", ".dds", ".jxr", ".wdp",
		};
		return extensions;
	}
}
