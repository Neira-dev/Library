#include "CAPMesh.h"
#include "CAPTexture.h"
#include "CapsuleAssetProtector.h"

#include <assimp/Importer.hpp>
#include <assimp/scene.h>
#include <assimp/postprocess.h>
#include <assimp/config.h>

#include <filesystem>
#include <map>

namespace cap
{
	namespace
	{
		constexpr uint32_t kMeshVersion = 1;
		using Vertex = CapsuleAssetProtector::Vertex;

		/**
		 * @brief マテリアルのテクスチャを解決し、CAPで読めるパス(または仮想パス)を返す
		 * @details 埋め込みテクスチャはここでクックしてパックへ保存する
		 */
		std::string ResolveTexture(const aiScene* scene, const aiMaterial* material,
			std::initializer_list<aiTextureType> types, const std::string& modelPath,
			const AddExtraAsset& addExtra, std::map<std::string, std::string>& resolved)
		{
			aiString texturePath;
			bool found = false;
			for (aiTextureType type : types)
			{
				if (material->GetTextureCount(type) > 0 && material->GetTexture(type, 0, &texturePath) == AI_SUCCESS)
				{
					found = true;
					break;
				}
			}
			if (!found || texturePath.length == 0)
			{
				return std::string();
			}

			const std::string key = texturePath.C_Str();
			auto cached = resolved.find(key);
			if (cached != resolved.end())
			{
				return cached->second;
			}

			std::string result;
			if (const aiTexture* embedded = scene->GetEmbeddedTexture(texturePath.C_Str()))
			{
				// モデルに埋め込まれたテクスチャ: 仮想パスでパックへ保存する
				const std::string virtualPath = modelPath + "#" + ToNarrow(FromUtf8(key));
				std::vector<uint8_t> cooked;
				bool ok = false;
				if (embedded->mHeight == 0)
				{
					// 圧縮画像(png/jpg等)がそのまま入っている
					ok = CookTextureFromMemory(embedded->pcData, embedded->mWidth, cooked);
				}
				else
				{
					// 非圧縮のピクセル(BGRA)が入っている
					const size_t pixelCount = static_cast<size_t>(embedded->mWidth) * embedded->mHeight;
					std::vector<uint8_t> rgba(pixelCount * 4);
					for (size_t i = 0; i < pixelCount; ++i)
					{
						rgba[i * 4 + 0] = embedded->pcData[i].r;
						rgba[i * 4 + 1] = embedded->pcData[i].g;
						rgba[i * 4 + 2] = embedded->pcData[i].b;
						rgba[i * 4 + 3] = embedded->pcData[i].a;
					}
					ok = CookTextureFromRgba(embedded->mWidth, embedded->mHeight, rgba.data(), cooked);
				}
				if (ok && addExtra(virtualPath, AssetType::Texture, cooked))
				{
					result = virtualPath;
				}
			}
			else
			{
				// 外部ファイル: モデルのあるフォルダからの相対パス
				namespace fs = std::filesystem;
				const fs::path modelDirectory = fs::path(ToWide(modelPath)).parent_path();
				const fs::path relative = FromUtf8(key);
				fs::path candidate = (modelDirectory / relative).lexically_normal();

				// 制作時のPCの絶対パスが入っていることがあるので、見つからなければファイル名だけで探す
				std::error_code ec;
				if (!fs::exists(candidate, ec))
				{
					const fs::path byName = modelDirectory / relative.filename();
					if (fs::exists(byName, ec))
					{
						candidate = byName;
					}
				}
				result = ToNarrow(candidate.wstring());
			}

			resolved[key] = result;
			return result;
		}

		std::unique_ptr<MeshAsset> BuildGpuMesh(CAPGpu& gpu, const std::vector<Vertex>& vertices,
			const std::vector<uint32_t>& indices, std::vector<SubMesh>&& subMeshes)
		{
			if (vertices.empty() || indices.empty())
			{
				SetError("メッシュの頂点またはインデックスが空です");
				return nullptr;
			}

			auto asset = std::make_unique<MeshAsset>();
			asset->type = AssetType::Mesh;

			const uint64_t vertexBytes = static_cast<uint64_t>(vertices.size()) * sizeof(Vertex);
			const uint64_t indexBytes = static_cast<uint64_t>(indices.size()) * sizeof(uint32_t);

			// 頂点とインデックスの転送を1回の送信・待機にまとめる
			if (!gpu.BeginBatch())
			{
				return nullptr;
			}
			const bool ok =
				gpu.CreateStaticBuffer(vertices.data(), vertexBytes, asset->vertexBuffer) &&
				gpu.CreateStaticBuffer(indices.data(), indexBytes, asset->indexBuffer);
			const bool submitted = gpu.EndBatch();
			if (!ok || !submitted)
			{
				return nullptr;
			}

			asset->vertexBufferView.BufferLocation = asset->vertexBuffer->GetGPUVirtualAddress();
			asset->vertexBufferView.StrideInBytes = sizeof(Vertex);
			asset->vertexBufferView.SizeInBytes = static_cast<UINT>(vertexBytes);

			asset->indexBufferView.BufferLocation = asset->indexBuffer->GetGPUVirtualAddress();
			asset->indexBufferView.Format = DXGI_FORMAT_R32_UINT;
			asset->indexBufferView.SizeInBytes = static_cast<UINT>(indexBytes);

			asset->subMeshes = std::move(subMeshes);
			asset->memoryBytes = vertexBytes + indexBytes;
			return asset;
		}
	}

	bool CookMesh(const std::string& path, const AddExtraAsset& addExtra, std::vector<uint8_t>& outCooked)
	{
		Assimp::Importer importer;
		// 点・線のプリミティブは描画しないので取り除く
		importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE, aiPrimitiveType_POINT | aiPrimitiveType_LINE);

		const unsigned int flags =
			aiProcess_Triangulate |            // 多角形を三角形に分割
			aiProcess_GenSmoothNormals |       // 法線が無ければ生成
			aiProcess_CalcTangentSpace |       // 法線マップ用の接線を生成
			aiProcess_JoinIdenticalVertices |  // 重複頂点をまとめてインデックス化
			aiProcess_PreTransformVertices |   // ノードの変換を頂点に焼き込む(静的メッシュ)
			aiProcess_SortByPType |            // プリミティブの種類ごとに分割
			aiProcess_ConvertToLeftHanded |    // DirectXの座標系・UV・面の向きに合わせる
			aiProcess_ImproveCacheLocality |   // GPUの頂点キャッシュ効率を上げる
			aiProcess_ValidateDataStructure;

		// assimpはUTF-8のパスを受け取る
		const aiScene* scene = importer.ReadFile(ToUtf8(ToWide(path)), flags);
		if (scene == nullptr || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) || scene->mRootNode == nullptr)
		{
			SetError("モデルの読み込みに失敗しました: " + path + "\n" + importer.GetErrorString());
			return false;
		}

		BinaryWriter writer;
		writer.Write<uint32_t>(kMeshVersion);

		// 部分メッシュ数は後で書き換える
		const size_t countOffset = writer.Buffer().size();
		writer.Write<uint32_t>(0);
		uint32_t subMeshCount = 0;

		std::map<std::string, std::string> resolvedTextures;
		for (unsigned int m = 0; m < scene->mNumMeshes; ++m)
		{
			const aiMesh* mesh = scene->mMeshes[m];
			if ((mesh->mPrimitiveTypes & aiPrimitiveType_TRIANGLE) == 0 || mesh->mNumVertices == 0)
			{
				continue;
			}

			std::vector<Vertex> vertices(mesh->mNumVertices);
			for (unsigned int v = 0; v < mesh->mNumVertices; ++v)
			{
				Vertex& out = vertices[v];
				out.position[0] = mesh->mVertices[v].x;
				out.position[1] = mesh->mVertices[v].y;
				out.position[2] = mesh->mVertices[v].z;

				if (mesh->HasNormals())
				{
					out.normal[0] = mesh->mNormals[v].x;
					out.normal[1] = mesh->mNormals[v].y;
					out.normal[2] = mesh->mNormals[v].z;
				}
				if (mesh->HasTextureCoords(0))
				{
					out.uv[0] = mesh->mTextureCoords[0][v].x;
					out.uv[1] = mesh->mTextureCoords[0][v].y;
				}
				if (mesh->HasTangentsAndBitangents() && mesh->HasNormals())
				{
					const aiVector3D& n = mesh->mNormals[v];
					const aiVector3D& t = mesh->mTangents[v];
					const aiVector3D& b = mesh->mBitangents[v];
					// 従法線の向き(UVが反転している面では-1になる)
					const aiVector3D c = n ^ t;
					out.tangent[0] = t.x;
					out.tangent[1] = t.y;
					out.tangent[2] = t.z;
					out.tangent[3] = ((c * b) < 0.0f) ? -1.0f : 1.0f;
				}
				if (mesh->HasVertexColors(0))
				{
					out.color[0] = mesh->mColors[0][v].r;
					out.color[1] = mesh->mColors[0][v].g;
					out.color[2] = mesh->mColors[0][v].b;
					out.color[3] = mesh->mColors[0][v].a;
				}
			}

			std::vector<uint32_t> indices;
			indices.reserve(static_cast<size_t>(mesh->mNumFaces) * 3);
			for (unsigned int f = 0; f < mesh->mNumFaces; ++f)
			{
				const aiFace& face = mesh->mFaces[f];
				if (face.mNumIndices != 3)
				{
					continue;
				}
				indices.push_back(face.mIndices[0]);
				indices.push_back(face.mIndices[1]);
				indices.push_back(face.mIndices[2]);
			}
			if (indices.empty())
			{
				continue;
			}

			std::string diffusePath;
			std::string normalPath;
			if (mesh->mMaterialIndex < scene->mNumMaterials)
			{
				const aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
				diffusePath = ResolveTexture(scene, material, { aiTextureType_BASE_COLOR, aiTextureType_DIFFUSE },
					path, addExtra, resolvedTextures);
				normalPath = ResolveTexture(scene, material, { aiTextureType_NORMALS, aiTextureType_NORMAL_CAMERA },
					path, addExtra, resolvedTextures);
			}

			writer.WriteString(ToNarrow(FromUtf8(mesh->mName.C_Str())));
			writer.WriteString(diffusePath);
			writer.WriteString(normalPath);
			writer.Write<uint32_t>(static_cast<uint32_t>(vertices.size()));
			writer.WriteBytes(vertices.data(), vertices.size() * sizeof(Vertex));
			writer.Write<uint32_t>(static_cast<uint32_t>(indices.size()));
			writer.WriteBytes(indices.data(), indices.size() * sizeof(uint32_t));
			++subMeshCount;
		}

		if (subMeshCount == 0)
		{
			SetError("モデルに描画できる三角形メッシュがありません: " + path);
			return false;
		}

		memcpy(writer.Buffer().data() + countOffset, &subMeshCount, sizeof(subMeshCount));
		outCooked = std::move(writer.Buffer());
		return true;
	}

	std::unique_ptr<MeshAsset> CreateMeshAsset(CAPGpu& gpu, const std::vector<uint8_t>& cooked)
	{
		BinaryReader reader(cooked);
		uint32_t version = 0;
		uint32_t subMeshCount = 0;
		if (!reader.Read(version) || version != kMeshVersion || !reader.Read(subMeshCount) || subMeshCount == 0)
		{
			SetError("メッシュのクック済みデータが壊れています");
			return nullptr;
		}

		// 全部分メッシュを1本の頂点/インデックスバッファにまとめる
		std::vector<Vertex> vertices;
		std::vector<uint32_t> indices;
		std::vector<SubMesh> subMeshes(subMeshCount);
		for (uint32_t i = 0; i < subMeshCount; ++i)
		{
			SubMesh& sub = subMeshes[i];
			uint32_t vertexCount = 0;
			uint32_t indexCount = 0;
			if (!reader.ReadString(sub.name) || !reader.ReadString(sub.diffusePath) || !reader.ReadString(sub.normalPath) ||
				!reader.Read(vertexCount))
			{
				SetError("メッシュのクック済みデータが壊れています");
				return nullptr;
			}
			const uint8_t* vertexData = reader.Take(static_cast<size_t>(vertexCount) * sizeof(Vertex));
			if (vertexData == nullptr || !reader.Read(indexCount))
			{
				SetError("メッシュのクック済みデータが壊れています");
				return nullptr;
			}
			const uint8_t* indexData = reader.Take(static_cast<size_t>(indexCount) * sizeof(uint32_t));
			if (indexData == nullptr)
			{
				SetError("メッシュのクック済みデータが壊れています");
				return nullptr;
			}

			sub.baseVertex = static_cast<int32_t>(vertices.size());
			sub.startIndex = static_cast<uint32_t>(indices.size());
			sub.indexCount = indexCount;

			const size_t vertexOffset = vertices.size();
			vertices.resize(vertexOffset + vertexCount);
			memcpy(vertices.data() + vertexOffset, vertexData, static_cast<size_t>(vertexCount) * sizeof(Vertex));

			const size_t indexOffset = indices.size();
			indices.resize(indexOffset + indexCount);
			memcpy(indices.data() + indexOffset, indexData, static_cast<size_t>(indexCount) * sizeof(uint32_t));
		}

		return BuildGpuMesh(gpu, vertices, indices, std::move(subMeshes));
	}

	std::unique_ptr<MeshAsset> CreateMeshFromData(CAPGpu& gpu, const Vertex* vertices, uint32_t vertexCount,
		const uint32_t* indices, uint32_t indexCount)
	{
		if (vertices == nullptr || indices == nullptr || vertexCount == 0 || indexCount == 0)
		{
			SetError("CreateMesh: 頂点またはインデックスが空です");
			return nullptr;
		}

		std::vector<Vertex> vertexArray(vertices, vertices + vertexCount);
		std::vector<uint32_t> indexArray(indices, indices + indexCount);

		std::vector<SubMesh> subMeshes(1);
		subMeshes[0].startIndex = 0;
		subMeshes[0].indexCount = indexCount;
		subMeshes[0].baseVertex = 0;
		return BuildGpuMesh(gpu, vertexArray, indexArray, std::move(subMeshes));
	}

	const std::vector<std::string>& MeshExtensions()
	{
		static const std::vector<std::string> extensions = { ".fbx", ".obj", ".gltf", ".glb" };
		return extensions;
	}
}
