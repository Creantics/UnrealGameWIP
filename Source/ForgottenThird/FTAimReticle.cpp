#include "FTAimReticle.h"

#include "Engine/Canvas.h"
#include "GameFramework/HUD.h"

namespace FT
{
void DrawAimReticle(AHUD& Hud, UCanvas* Canvas, float Alpha, bool bOutOfReach)
{
	if (!Canvas || Alpha <= 0.f)
	{
		return;
	}
	const float CX = Canvas->ClipX * 0.5f;
	const float CY = Canvas->ClipY * 0.5f;
	const float R = FMath::Max(6.f, Canvas->ClipY * 0.0075f); // the ring's radius: 1.5% of the height across
	const float Line = FMath::Max(1.5f, Canvas->ClipY * 0.0016f);
	const float Dot = FMath::Max(1.5f, Canvas->ClipY * 0.0015f);
	const float Faint = bOutOfReach ? 0.35f : 1.f;
	const FLinearColor Dark(0.f, 0.f, 0.f, 0.6f * Alpha * Faint);
	const FLinearColor Light(1.f, 1.f, 1.f, 0.95f * Alpha * Faint);
	constexpr int32 Segments = 24;
	// The outline first (wider, dark), then the white on top.
	for (int32 Pass = 0; Pass < 2; ++Pass)
	{
		const bool bDark = Pass == 0;
		const FLinearColor& Col = bDark ? Dark : Light;
		const float T = bDark ? Line + 2.f : Line;
		const float D = bDark ? Dot + 1.f : Dot;
		if (!bOutOfReach)
		{
			Hud.DrawRect(Col, CX - D, CY - D, 2.f * D, 2.f * D);
		}
		float PX = CX + R;
		float PY = CY;
		for (int32 I = 1; I <= Segments; ++I)
		{
			const float A = 2.f * PI * static_cast<float>(I) / static_cast<float>(Segments);
			const float NX = CX + R * FMath::Cos(A);
			const float NY = CY + R * FMath::Sin(A);
			Hud.DrawLine(PX, PY, NX, NY, Col, T);
			PX = NX;
			PY = NY;
		}
	}
}
} // namespace FT
