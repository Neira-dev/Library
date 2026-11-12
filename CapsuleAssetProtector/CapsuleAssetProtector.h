#pragma once

// =============================================
// CapsuleAssetProtector
// =============================================
// アセットの管理・保護を行うクラスです。
// アセットの読み込み、バイナリ化、暗号化、復号化などを行います。
// 
// Version: 1.0.0

// =============================================
/*
*	更新履歴
*/
// =============================================

namespace CapsuleAssetProtector
{
	/**
	 * @brief CapsuleAssetProtectorの初期化処理を行います。
	 */
	void Initialize();

	/**
	 * @brief CapsuleAssetProtectorの終了処理を行います。
	 */
	void Shutdown();
};