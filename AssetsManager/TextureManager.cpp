#include "TextureManager.h"

TextureManager* TextureManager::_instance = nullptr;

TextureManager* TextureManager::Singleton()
{
	if (_instance == nullptr)
	{
		_instance = new TextureManager();
	}
	return _instance;
}

void TextureManager::DestroySingleton()
{
	if (_instance != nullptr)
	{
		delete _instance;
		_instance = nullptr;
	}
}