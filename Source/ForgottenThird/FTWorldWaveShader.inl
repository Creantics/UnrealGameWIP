// HLSL bodies of the world wave's Custom nodes (FTBuildWaveMaterialsCommandlet): the cut in MF_WorldWaveCut, the surface
// state in MF_WorldWaveState and the post-process material /Game/Look/Wave/PP_WorldWave. Their functions live in the
// FTLook plugin's shaders (Plugins/FTLook/Shaders/Private/Wave: Common.ush, State.ush, Post.ush), which the nodes
// include, so the ragged edge a thing is clipped on is the edge a surface changes on and the post-process masks by.
// Editing those files recompiles the materials on their next load with no rebuild. Distances in cm.

static const TCHAR* const FTWorldWaveCommonInclude = TEXT("/Plugin/FTLook/Private/Wave/Common.ush");
static const TCHAR* const FTWorldWaveStateInclude = TEXT("/Plugin/FTLook/Private/Wave/State.ush");
static const TCHAR* const FTWorldWavePostInclude = TEXT("/Plugin/FTLook/Private/Wave/Post.ush");

// The cut, in MF_WorldWaveCut. Inputs: WorldPos (absolute world position), WOrigin, WRadius, WBand, WDirection, WTime
// (the collection) and Tag, LocalR (custom primitive data, FTWorldWave::kTagSlot and kLocalSlot). Returns the clip (1
// keep, 0 cut). No derivatives: ray-tracing hit shaders compile this too.
static const TCHAR* const FTWorldWaveCutBody =
TEXT(R"HLSL(
float tag = Tag;
if (tag < 0.5)
{
	return 1.0;
}
FTWaveFns W;
float3 rel = WorldPos - WOrigin.xyz;
float dist = length(rel);
float3 dir = rel / max(dist, 0.01);
float r = LocalR > 0.0 ? LocalR : WRadius;
float w = max(WBand, 1.0);
bool forward = WDirection < 0.5;
bool oldOnly = tag < 1.5;
float edge = W.Edge(r, w, dir, WTime, WorldPos);
// The thing the wave takes away (OldOnly going forward, NewOnly in reverse) is taken from the side the front reaches
// first, the taken part running out in thin radial slivers across the rest of the band; the thing it brings is drawn in
// from the same side.
bool leaving = (oldOnly == forward);
if (leaving)
{
	float reach = smoothstep(0.62, 0.9, W.Streak(dir)) * max(r - edge, 0.0);
	return dist >= edge + reach ? 1.0 : 0.0;
}
return dist < edge ? 1.0 : 0.0;
)HLSL");

// The surface state, in MF_WorldWaveState. Inputs: Base, Rough, Emis (the master's own), WorldPos, Normal (vertex
// normal, world), WOrigin (w: the front's look), WOriginScreen (w: a wave is set), WRadius, WBand, WDirection, WTime,
// WKeep0, WKeep1 (the collection). Returns the base colour; RoughOut and EmisOut the roughness and emissive. The work is
// FTWaveSurface (State.ush), so a change to it needs no rebuild.
static const TCHAR* const FTWorldWaveStateBody =
TEXT(R"HLSL(
float3 base = Base;
float rough = Rough;
float3 emis = Emis;
FTWaveSurface(WorldPos, Normal, WOrigin, WOriginScreen.w, WRadius, WBand, WDirection, WTime, WKeep0, WKeep1, base, rough, emis);
RoughOut = rough;
EmisOut = emis;
return base;
)HLSL");

// The same state on a character (MF_WorldWaveState with Character on): the inputs above with LocalPos (pre-skinned, cm)
// in place of Normal. The work is FTWaveCharacter (State.ush).
static const TCHAR* const FTWorldWaveCharacterBody =
TEXT(R"HLSL(
float3 base = Base;
float rough = Rough;
float3 emis = Emis;
FTWaveCharacter(WorldPos, LocalPos, WOrigin, WOriginScreen.w, WRadius, WBand, WDirection, WTime, WKeep0, WKeep1, base, rough, emis);
RoughOut = rough;
EmisOut = emis;
return base;
)HLSL");

// The post-process (before depth of field, linear scene colour). Inputs: SceneColor (PostProcessInput0), SceneDepthIn
// (SceneDepth, cm), SceneNormal (the world normal), WorldPos, CamPos, the collection's WOrigin, WOriginScreen, WFoot,
// WTip, WRadius, WBand, WPrelude, WDirection, WTime, WGround, WGain, WKeep0 (the composed light), and the material's
// SkyDepth. The work is FTWavePostMain (Post.ush), so a change to it needs no rebuild.
static const TCHAR* const FTWorldWavePostBody =
TEXT(R"HLSL(
return FTWavePostMain(Parameters, SceneColor.rgb, SceneDepthIn.r, SceneNormal.xyz, WorldPos, CamPos, WOrigin, WOriginScreen,
	WFoot, WTip, WRadius, WBand, WPrelude, WDirection, WTime, WGround, WGain, WKeep0, SkyDepth);
)HLSL");

