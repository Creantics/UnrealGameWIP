#pragma once

// The keys and the glyph of ftcore::ActionButton (Core/include/ftcore/ActionButton.h), shared by every
// screen whose prompts are answered with the action buttons: the quick-time event, the mini-game and the
// Beat fight. Arrows (the arrow keys, WASD, the d-pad) belong to the Turns battle's combos alone and are
// not read here.
//
// The order is ActionButton's: the pad's face buttons clockwise from the left, and the keyboard's I, J,
// K, L diamond in the same layout.
//   Attack  pad face left    J
//   Guard   pad face top     I
//   Dodge   pad face right   L
//   Jump    pad face bottom  K

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "InputCoreTypes.h"

#include "ftcore/ActionButton.h"

namespace FTActionButtons
{
constexpr int32 Count = static_cast<int32>(ftcore::kActionButtonCount);
constexpr int32 KeysPerButton = 2;

inline const FKey& Key(int32 Button, int32 Which)
{
	static const FKey Keys[Count][KeysPerButton] = {
		{EKeys::Gamepad_FaceButton_Left, EKeys::J},
		{EKeys::Gamepad_FaceButton_Top, EKeys::I},
		{EKeys::Gamepad_FaceButton_Right, EKeys::L},
		{EKeys::Gamepad_FaceButton_Bottom, EKeys::K},
	};
	return Keys[Button & 3][Which & 1];
}

inline ftcore::ActionButton Button(int32 Index)
{
	return static_cast<ftcore::ActionButton>(Index & 3);
}

// "Attack  J" and so on, under a prompt. Static, so drawing never builds strings.
inline const FString& Label(int32 Button)
{
	static const FString Labels[Count] = {FString(TEXT("Attack  J")), FString(TEXT("Guard  I")),
		FString(TEXT("Dodge  L")), FString(TEXT("Jump  K"))};
	return Labels[Button & 3];
}

// Where each button sits in the face-button diamond, in screen space.
inline FVector2D Place(int32 Button)
{
	static const FVector2D Places[Count] = {FVector2D(-1, 0), FVector2D(0, -1), FVector2D(1, 0), FVector2D(0, 1)};
	return Places[Button & 3];
}

// The face-button diamond centred on (X, Y): four outlined squares, the prompt's button filled.
inline void DrawGlyph(AHUD& Hud, int32 Button, float X, float Y, float Radius, const FLinearColor& Color, float Thickness)
{
	const float Half = Radius * 0.3f;
	for (int32 B = 0; B < Count; ++B)
	{
		const FVector2D C = FVector2D(X, Y) + Place(B) * Radius * 0.62f;
		if (B == (Button & 3))
		{
			Hud.DrawRect(Color, C.X - Half, C.Y - Half, Half * 2.f, Half * 2.f);
			continue;
		}
		const float T = Thickness * 0.5f;
		const FLinearColor Dim(Color.R, Color.G, Color.B, Color.A * 0.45f);
		Hud.DrawLine(C.X - Half, C.Y - Half, C.X + Half, C.Y - Half, Dim, T);
		Hud.DrawLine(C.X + Half, C.Y - Half, C.X + Half, C.Y + Half, Dim, T);
		Hud.DrawLine(C.X + Half, C.Y + Half, C.X - Half, C.Y + Half, Dim, T);
		Hud.DrawLine(C.X - Half, C.Y + Half, C.X - Half, C.Y - Half, Dim, T);
	}
}
} // namespace FTActionButtons
