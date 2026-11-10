#pragma once
class Scene
{
public:
	Scene() = default;
	~Scene() = default;
public:
	void Initialize();
	void Update();
	void Render();
	void UnLoad();
};

