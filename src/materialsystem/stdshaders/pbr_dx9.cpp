//==================================================================================================
//
// Physically Based Rendering shader for brushes and models
//
//==================================================================================================

// Includes for all shaders
#include "BaseVSShader.h"
#include "cpp_shader_constant_register_map.h"

// Includes for PS30
#include "pbr_vs30.inc"
#include "pbr_ps30.inc"

// Includes for PS20b
#include "pbr_vs20.inc"
#include "pbr_ps20b.inc"

// Defining samplers
constexpr Sampler_t SAMPLER_BASETEXTURE = SHADER_SAMPLER0;
constexpr Sampler_t SAMPLER_BASETEXTURE2 = SHADER_SAMPLER3;
constexpr Sampler_t SAMPLER_NORMAL = SHADER_SAMPLER1;
constexpr Sampler_t SAMPLER_NORMAL2 = SHADER_SAMPLER12;
constexpr Sampler_t SAMPLER_ENVMAP = SHADER_SAMPLER2;
constexpr Sampler_t SAMPLER_SHADOWDEPTH = SHADER_SAMPLER4;
constexpr Sampler_t SAMPLER_RANDOMROTATION = SHADER_SAMPLER5;
constexpr Sampler_t SAMPLER_FLASHLIGHT = SHADER_SAMPLER6;
constexpr Sampler_t SAMPLER_LIGHTMAP = SHADER_SAMPLER7;
constexpr Sampler_t SAMPLER_MRAO = SHADER_SAMPLER10;
constexpr Sampler_t SAMPLER_MRAO2 = SHADER_SAMPLER13;
constexpr Sampler_t SAMPLER_EMISSIVE = SHADER_SAMPLER11;
constexpr Sampler_t SAMPLER_EMISSIVE2 = SHADER_SAMPLER14;
constexpr Sampler_t SAMPLER_BRDF_INTEGRATION = SHADER_SAMPLER8;

constexpr int PARALLAX_QUALITY_MAX = 3;

// Convars
static ConVar mat_fullbright( "mat_fullbright", "0", FCVAR_CHEAT );
static ConVar mat_pbr_force_20b( "mat_pbr_force_20b", "0", FCVAR_CHEAT );
static ConVar mat_pbr_parallaxmap( "mat_pbr_parallaxmap", "1" );
static ConVar mat_pbr_parallaxmap_quality( "mat_pbr_parallaxmap_quality", "3", FCVAR_NONE, "", true, 0, true, PARALLAX_QUALITY_MAX );
extern ConVar mat_allow_parallax_cubemaps;

static ConVar mat_pbr_parallax_dither_amount( "mat_pbr_parallax_dither_amount", "1", FCVAR_NONE, "The level to which pbr parallax dithering can increase steps. Higher numbers are more performance-heavy.", true, 0.f, false, 0.f );

static constexpr int PARALLAX_SAMPLES[] =
{
	16,
	32,
	64,
	128
};
COMPILE_TIME_ASSERT( ARRAYSIZE( PARALLAX_SAMPLES ) == PARALLAX_QUALITY_MAX + 1 );

static float s_flLightmapScale = 1.0f;

// Beginning the shader
BEGIN_VS_SHADER( PBR, "Physically Based Rendering shader for brushes and models" )
	// Setting up vmt parameters
	BEGIN_SHADER_PARAMS;
		SHADER_PARAM(BASETEXTURE2, SHADER_PARAM_TYPE_TEXTURE, "shadertest/lightmappedtexture", "Blended texture")
		SHADER_PARAM(FRAME2, SHADER_PARAM_TYPE_INTEGER, "0", "frame number for $basetexture2")
		SHADER_PARAM(ALPHATESTREFERENCE, SHADER_PARAM_TYPE_FLOAT, "0", "")
		SHADER_PARAM(ENVMAP, SHADER_PARAM_TYPE_TEXTURE, "", "Set the cubemap for this material.")
		SHADER_PARAM(ENVMAPFRAME, SHADER_PARAM_TYPE_INTEGER, "", "Frame number for $envmap.")
		SHADER_PARAM(MRAOTEXTURE, SHADER_PARAM_TYPE_TEXTURE, "", "Texture with metalness in R, roughness in G, ambient occlusion in B.")
		SHADER_PARAM(MRAOFRAME, SHADER_PARAM_TYPE_INTEGER, "", "Frame number for $mraotexture.")
		SHADER_PARAM(MRAOTEXTURE2, SHADER_PARAM_TYPE_TEXTURE, "", "Texture with metalness in R, roughness in G, ambient occlusion in B.")
		SHADER_PARAM(MRAOFRAME2, SHADER_PARAM_TYPE_INTEGER, "", "Frame number for $mraotexture2.")
		SHADER_PARAM(EMISSIONTEXTURE, SHADER_PARAM_TYPE_TEXTURE, "", "Emission texture")
		SHADER_PARAM(EMISSIONFRAME, SHADER_PARAM_TYPE_INTEGER, "", "Frame number for $emissiontexture.")
		SHADER_PARAM(EMISSIONTEXTURE2, SHADER_PARAM_TYPE_TEXTURE, "", "Emission texture")
		SHADER_PARAM(EMISSIONFRAME2, SHADER_PARAM_TYPE_INTEGER, "", "Frame number for $emissiontexture2.")
		SHADER_PARAM(EMISSIONFRESNEL, SHADER_PARAM_TYPE_VEC4, "[1 1 1 2]", "Fresnel ranges and exponent for emissive texture blending")
		SHADER_PARAM(NORMALTEXTURE, SHADER_PARAM_TYPE_TEXTURE, "", "Normal texture (deprecated, use $bumpmap)")
		SHADER_PARAM(BUMPMAP, SHADER_PARAM_TYPE_TEXTURE, "", "Normal texture")
		SHADER_PARAM(BUMPFRAME, SHADER_PARAM_TYPE_INTEGER, "", "Frame number for $bumpmap.")
		SHADER_PARAM(BUMPMAP2, SHADER_PARAM_TYPE_TEXTURE, "", "Normal texture")
		SHADER_PARAM(BUMPFRAME2, SHADER_PARAM_TYPE_INTEGER, "", "Frame number for $bumpmap2.")
		SHADER_PARAM(PARALLAX, SHADER_PARAM_TYPE_BOOL, "0", "Use Parallax Occlusion Mapping.")
		SHADER_PARAM(PARALLAXDEPTH, SHADER_PARAM_TYPE_FLOAT, "0.0030", "Depth of the Parallax Map")
		SHADER_PARAM(PARALLAXCENTER, SHADER_PARAM_TYPE_FLOAT, "0.5", "Center depth of the Parallax Map")
		SHADER_PARAM(MRAOSCALE, SHADER_PARAM_TYPE_COLOR, "[1 1 1]", "Factors for metalness, roughness, and ambient occlusion")
		SHADER_PARAM(MRAOSCALE2, SHADER_PARAM_TYPE_COLOR, "[1 1 1]", "Factors for metalness, roughness, and ambient occlusion")
		SHADER_PARAM(EMISSIONSCALE, SHADER_PARAM_TYPE_COLOR, "[1 1 1]", "Color to multiply emission texture with")
		SHADER_PARAM(EMISSIONSCALE2, SHADER_PARAM_TYPE_COLOR, "[1 1 1]", "Color to multiply emission texture with")
		SHADER_PARAM(HSV, SHADER_PARAM_TYPE_COLOR, "[1 1 1]", "HSV color to transform $basetexture texture with")
		SHADER_PARAM(HSV_BLEND, SHADER_PARAM_TYPE_BOOL, "0", "Blend untransformed color and HSV transformed color")
		SHADER_PARAM(BRDF_INTEGRATION, SHADER_PARAM_TYPE_TEXTURE, "", "")
		SHADER_PARAM(ENVMAPPARALLAX, SHADER_PARAM_TYPE_BOOL, "0", "Should envmap reflections be parallax-corrected?")
		SHADER_PARAM(ENVMAPPARALLAXOBB1, SHADER_PARAM_TYPE_VEC4, "[1 0 0 0]", "The first line of the parallax correction OBB matrix")
		SHADER_PARAM(ENVMAPPARALLAXOBB2, SHADER_PARAM_TYPE_VEC4, "[0 1 0 0]", "The second line of the parallax correction OBB matrix")
		SHADER_PARAM(ENVMAPPARALLAXOBB3, SHADER_PARAM_TYPE_VEC4, "[0 0 1 0]", "The third line of the parallax correction OBB matrix")
		SHADER_PARAM(ENVMAPORIGIN, SHADER_PARAM_TYPE_VEC3, "[0 0 0]", "The world space position of the env_cubemap being corrected")
		SHADER_PARAM(BLENDTINTBYMRAOALPHA, SHADER_PARAM_TYPE_BOOL, "0", "Blend tint by the alpha channel in MRAO texture. Similar to $blendtintbybasealpha for VLG")
		SHADER_PARAM(PARALLAXDITHER, SHADER_PARAM_TYPE_BOOL, "0", "When enabled, apply dithering to parallax to improve quality")
		SHADER_PARAM(PARALLAXSCALE, SHADER_PARAM_TYPE_FLOAT, "1", "Multiply the number of parallax samples by this amount. DANGER: This can be EXPENSIVE!")
	END_SHADER_PARAMS;

	// Initializing parameters
	SHADER_INIT_PARAMS()
	{
		// Fallback for changed parameter
		if (params[NORMALTEXTURE]->IsDefined())
			params[BUMPMAP]->SetStringValue(params[NORMALTEXTURE]->GetStringValue());

		// Dynamic lights need a bumpmap
		if (!params[BUMPMAP]->IsDefined())
			params[BUMPMAP]->SetStringValue("dev/flat_normal");

		if (!params[BUMPMAP2]->IsDefined())
			params[BUMPMAP2]->SetStringValue("dev/flat_normal");

		// Set a good default mrao texture
		if (!params[MRAOTEXTURE]->IsDefined())
			params[MRAOTEXTURE]->SetStringValue("dev/pbr_mraotexture");

		if (!params[MRAOTEXTURE2]->IsDefined())
			params[MRAOTEXTURE2]->SetStringValue("dev/pbr_mraotexture");

		// PBR relies heavily on envmaps
		if (!params[ENVMAP]->IsDefined())
			params[ENVMAP]->SetStringValue("env_cubemap");

		if (!params[BRDF_INTEGRATION]->IsDefined())
			params[BRDF_INTEGRATION]->SetStringValue("dev/brdf_integration");

		if (!params[FRAME2]->IsDefined())
			params[FRAME2]->SetIntValue(0);
		if (!params[BUMPFRAME]->IsDefined())
			params[BUMPFRAME]->SetIntValue(0);
		if (!params[ENVMAPFRAME]->IsDefined())
			params[ENVMAPFRAME]->SetIntValue(0);
		if (!params[BUMPFRAME2]->IsDefined())
			params[BUMPFRAME2]->SetIntValue(0);
		if (!params[MRAOFRAME]->IsDefined())
			params[MRAOFRAME]->SetIntValue(0);
		if (!params[MRAOFRAME2]->IsDefined())
			params[MRAOFRAME2]->SetIntValue(0);
		if (!params[EMISSIONFRAME]->IsDefined())
			params[EMISSIONFRAME]->SetIntValue(0);
		if (!params[EMISSIONFRAME2]->IsDefined())
			params[EMISSIONFRAME2]->SetIntValue(0);
		if (!params[PARALLAXSCALE]->IsDefined())
			params[PARALLAXSCALE]->SetFloatValue(1.0f);
		if (!params[PARALLAXDITHER]->IsDefined())
			params[PARALLAXDITHER]->SetIntValue(0);

		if (!params[MRAOSCALE]->IsDefined())
			params[MRAOSCALE]->SetVecValue(-1, -1, -1);
		if (!params[MRAOSCALE2]->IsDefined() )
			params[MRAOSCALE2]->SetVecValue(-1, -1, -1);
		if (!params[EMISSIONFRESNEL]->IsDefined())
			params[EMISSIONFRESNEL]->SetVecValue(-1, -1, -1, -1);
		if (!params[EMISSIONSCALE]->IsDefined())
			params[EMISSIONSCALE]->SetVecValue(-1, -1, -1);
		if (!params[EMISSIONSCALE2]->IsDefined() )
			params[EMISSIONSCALE2]->SetVecValue(-1, -1, -1);
		if (!params[HSV]->IsDefined())
			params[HSV]->SetVecValue(-1, -1, -1);
		if (!params[BLENDTINTBYMRAOALPHA]->IsDefined())
			params[BLENDTINTBYMRAOALPHA]->SetIntValue(0);
	}

	// Define shader fallback
	SHADER_FALLBACK
	{
		return 0;
	};

	SHADER_INIT
	{
		LoadBumpMap( BUMPMAP );
		LoadBumpMap( BUMPMAP2 );
		LoadCubeMap( ENVMAP );

		if ( params[EMISSIONTEXTURE]->IsDefined() )
			LoadTexture( EMISSIONTEXTURE );

		if ( params[EMISSIONTEXTURE2]->IsDefined() )
			LoadTexture( EMISSIONTEXTURE2 );

		LoadTexture( MRAOTEXTURE );
		LoadTexture( MRAOTEXTURE2 );
		LoadTexture( BRDF_INTEGRATION );

		if ( params[BASETEXTURE]->IsDefined() )
		{
			LoadTexture( BASETEXTURE );
		}

		if ( params[BASETEXTURE2]->IsDefined() )
		{
			LoadTexture( BASETEXTURE2 );
		}

		SET_FLAGS2(MATERIAL_VAR2_SUPPORTS_HW_SKINNING);
		if (IS_FLAG_SET(MATERIAL_VAR_MODEL)) // Set material var2 flags specific to models
		{
			SET_FLAGS2(MATERIAL_VAR2_DIFFUSE_BUMPMAPPED_MODEL);         // Required for dynamic lighting
			SET_FLAGS2(MATERIAL_VAR2_NEEDS_TANGENT_SPACES);             // Required for dynamic lighting
			SET_FLAGS2(MATERIAL_VAR2_LIGHTING_VERTEX_LIT);              // Required for dynamic lighting
			SET_FLAGS2(MATERIAL_VAR2_NEEDS_BAKED_LIGHTING_SNAPSHOTS);   // Required for ambient cube
			SET_FLAGS2(MATERIAL_VAR2_SUPPORTS_FLASHLIGHT);              // Required for flashlight
			SET_FLAGS2(MATERIAL_VAR2_USE_FLASHLIGHT);                   // Required for flashlight
		}
		else // Set material var2 flags specific to brushes
		{
			SET_FLAGS2(MATERIAL_VAR2_LIGHTING_LIGHTMAP);                // Required for lightmaps
			SET_FLAGS2(MATERIAL_VAR2_LIGHTING_BUMPED_LIGHTMAP);         // Required for lightmaps
			SET_FLAGS2(MATERIAL_VAR2_SUPPORTS_FLASHLIGHT);              // Required for flashlight
			SET_FLAGS2(MATERIAL_VAR2_USE_FLASHLIGHT);                   // Required for flashlight
		}

		// Cubemap parallax correction requires all 4 lines
		if ( !mat_allow_parallax_cubemaps.GetBool() || !params[ENVMAPPARALLAX]->IsDefined() || !params[ENVMAPPARALLAXOBB1]->IsDefined() || !params[ENVMAPPARALLAXOBB2]->IsDefined() || !params[ENVMAPPARALLAXOBB3]->IsDefined() || !params[ENVMAPORIGIN]->IsDefined() )
		{
			params[ENVMAPPARALLAX]->SetIntValue( 0 );
		}
	};

	// Drawing the shader
	SHADER_DRAW
	{
		// Setting up booleans
		const bool bHasBaseTexture = params[BASETEXTURE]->IsTexture();
		const bool bIsWVT = !IS_FLAG_SET( MATERIAL_VAR_MODEL ) && params[BASETEXTURE2]->IsTexture();
		const bool bHasNormalTexture = params[BUMPMAP]->IsTexture();
		const bool bHasNormalTexture2 = params[BUMPMAP2]->IsTexture();
		const bool bHasMraoTexture = params[MRAOTEXTURE]->IsTexture();
		const bool bHasMraoTexture2 = params[MRAOTEXTURE2]->IsTexture();
		const bool bHasEmissionTexture = params[EMISSIONTEXTURE]->IsTexture();
		const bool bHasEmissionTexture2 = params[EMISSIONTEXTURE2]->IsTexture();
		const bool bHasEnvTexture = params[ENVMAP]->IsTexture();
		const bool bIsAlphaTested = IS_FLAG_SET( MATERIAL_VAR_ALPHATEST ) != 0;
		const bool bHasFlashlight = UsingFlashlight( params );
		const bool bLightMapped = !IS_FLAG_SET( MATERIAL_VAR_MODEL );
		const bool bHasMraoScale = params[MRAOSCALE]->GetVecValue()[0] >= 0.0f;
		const bool bHasMraoScale2 = params[MRAOSCALE2]->GetVecValue()[0] >= 0.0f;
		const bool bHasEmissionScale = params[EMISSIONSCALE]->GetVecValue()[0] >= 0.0f;
		const bool bHasEmissionScale2 = params[EMISSIONSCALE2]->GetVecValue()[0] >= 0.0f;
		const bool bHasEmissionFresnel = g_pHardwareConfig->HasFastVertexTextures() && bHasEmissionTexture && !bHasFlashlight && params[EMISSIONFRESNEL]->GetVecValue()[3] >= 0.0f;
		const bool bHasHSV = params[HSV]->GetVecValue()[0] >= 0.0f;
		const bool bBlendHSV = bHasHSV && IsBoolSet( HSV_BLEND, params );
		const bool bHasParallaxCorrection = bHasEnvTexture && !!params[ENVMAPPARALLAX]->GetIntValue();
		const bool bBlendTintByMRAOAlpha = !!params[BLENDTINTBYMRAOALPHA]->GetIntValue();

		// Determining whether we're dealing with a fully opaque material
		const BlendType_t nBlendType = EvaluateBlendRequirements(BASETEXTURE, true);
		const bool bFullyOpaque = (nBlendType != BT_BLENDADD) && (nBlendType != BT_BLEND) && !bIsAlphaTested;

		const bool useParallax = mat_pbr_parallaxmap.GetBool() && g_pHardwareConfig->HasFastVertexTextures() && !mat_pbr_force_20b.GetBool() && !!params[PARALLAX]->GetIntValue();
		const bool bParallaxDither = useParallax && !!params[PARALLAXDITHER]->GetIntValue();

		if (IsSnapshotting())
		{
			// If alphatest is on, enable it
			pShaderShadow->EnableAlphaTest(bIsAlphaTested);

			if (params[ALPHATESTREFERENCE]->GetFloatValue() > 0.0f)
			{
				pShaderShadow->AlphaFunc(SHADER_ALPHAFUNC_GEQUAL, params[ALPHATESTREFERENCE]->GetFloatValue());
			}

			if (bHasFlashlight)
			{
				pShaderShadow->EnableDepthWrites(false);
				pShaderShadow->EnableAlphaWrites(false);

				SetAdditiveBlendingShadowState(BASETEXTURE, true);
			}
			else
			{
				// HACK HACK HACK - enable alpha writes all the time so that we have them for
				// underwater stuff and writing depth to dest alpha
				// But only do it if we're not using the alpha already for translucency
				pShaderShadow->EnableAlphaWrites(bFullyOpaque);

				SetDefaultBlendingShadowState(BASETEXTURE, true);
			}

			int nShadowFilterMode = bHasFlashlight ? g_pHardwareConfig->GetShadowFilterMode() : 0;

			unsigned int nFlags = VERTEX_POSITION | VERTEX_NORMAL | VERTEX_FORMAT_COMPRESSED;

			// Valid for static props
			if ( !bLightMapped )
				nFlags |= VERTEX_COLOR_STREAM_1;

			int nTexCoordCount = 1;

			// We need another 2 texcoords for lightmap data
			if ( bLightMapped )
				nTexCoordCount += 2;

			// WVT blend amount is in alpha channel of color
			if ( bIsWVT )
				nFlags |= VERTEX_COLOR;

			pShaderShadow->VertexShaderVertexFormat( nFlags, nTexCoordCount, nullptr, 0 );

			if (!g_pHardwareConfig->HasFastVertexTextures() || mat_pbr_force_20b.GetBool())
			{
				// Setting up static vertex shader
				DECLARE_STATIC_VERTEX_SHADER(pbr_vs20);
				SET_STATIC_VERTEX_SHADER_COMBO(WVT, bIsWVT);
				SET_STATIC_VERTEX_SHADER_COMBO(LIGHTMAPPED, bLightMapped);
				SET_STATIC_VERTEX_SHADER(pbr_vs20);

				// Setting up static pixel shader
				DECLARE_STATIC_PIXEL_SHADER(pbr_ps20b);
				SET_STATIC_PIXEL_SHADER_COMBO(FLASHLIGHT, bHasFlashlight);
				SET_STATIC_PIXEL_SHADER_COMBO(FLASHLIGHTDEPTHFILTERMODE, nShadowFilterMode);
				SET_STATIC_PIXEL_SHADER_COMBO(LIGHTMAPPED, bLightMapped);
				SET_STATIC_PIXEL_SHADER_COMBO(EMISSIVE, bHasEmissionTexture);
				SET_STATIC_PIXEL_SHADER_COMBO(PARALLAXOCCLUSION, useParallax);
				SET_STATIC_PIXEL_SHADER_COMBO(WVT, bIsWVT);
				SET_STATIC_PIXEL_SHADER_COMBO(HSV, bHasHSV);
				SET_STATIC_PIXEL_SHADER_COMBO(HSV_BLEND, bBlendHSV);
				SET_STATIC_PIXEL_SHADER_COMBO(PARALLAXCORRECT, bHasParallaxCorrection);
				SET_STATIC_PIXEL_SHADER_COMBO(BLENDTINTBYMRAOALPHA, bBlendTintByMRAOAlpha);
				SET_STATIC_PIXEL_SHADER_COMBO(PARALLAXDITHER, bParallaxDither);
				SET_STATIC_PIXEL_SHADER_COMBO(EMISSIVEFRESNEL, bHasEmissionFresnel);
				SET_STATIC_PIXEL_SHADER(pbr_ps20b);
			}
			else
			{
				// Setting up static vertex shader
				DECLARE_STATIC_VERTEX_SHADER(pbr_vs30);
				SET_STATIC_VERTEX_SHADER_COMBO(WVT, bIsWVT);
				SET_STATIC_VERTEX_SHADER_COMBO(LIGHTMAPPED, bLightMapped);
				SET_STATIC_VERTEX_SHADER(pbr_vs30);

				// Setting up static pixel shader
				DECLARE_STATIC_PIXEL_SHADER(pbr_ps30);
				SET_STATIC_PIXEL_SHADER_COMBO(FLASHLIGHT, bHasFlashlight);
				SET_STATIC_PIXEL_SHADER_COMBO(FLASHLIGHTDEPTHFILTERMODE, nShadowFilterMode);
				SET_STATIC_PIXEL_SHADER_COMBO(LIGHTMAPPED, bLightMapped);
				SET_STATIC_PIXEL_SHADER_COMBO(EMISSIVE, bHasEmissionTexture);
				SET_STATIC_PIXEL_SHADER_COMBO(PARALLAXOCCLUSION, useParallax);
				SET_STATIC_PIXEL_SHADER_COMBO(WVT, bIsWVT);
				SET_STATIC_PIXEL_SHADER_COMBO(HSV, bHasHSV);
				SET_STATIC_PIXEL_SHADER_COMBO(HSV_BLEND, bBlendHSV);
				SET_STATIC_PIXEL_SHADER_COMBO(PARALLAXCORRECT, bHasParallaxCorrection);
				SET_STATIC_PIXEL_SHADER_COMBO(BLENDTINTBYMRAOALPHA, bBlendTintByMRAOAlpha);
				SET_STATIC_PIXEL_SHADER_COMBO(PARALLAXDITHER, bParallaxDither);
				SET_STATIC_PIXEL_SHADER_COMBO(EMISSIVEFRESNEL, bHasEmissionFresnel);
				SET_STATIC_PIXEL_SHADER(pbr_ps30);
			}

			if (bHasFlashlight)
			{
				FogToBlack();
			}
			else
			{
				DefaultFog();
			}

			pShaderShadow->EnableSRGBWrite( true );

			pShaderShadow->EnableTexture( SHADER_SAMPLER0, true ); // base texture
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER0, true );
			pShaderShadow->EnableTexture( SHADER_SAMPLER1, true ); // normal map
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER1, false );
			pShaderShadow->EnableTexture( SHADER_SAMPLER2, true ); // cubemap
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER2, true );
			pShaderShadow->EnableTexture( SHADER_SAMPLER5, true );	// flashlight depth map
			pShaderShadow->SetShadowDepthFiltering( SHADER_SAMPLER4 );
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER4, false );
			pShaderShadow->EnableTexture( SHADER_SAMPLER5, true );	// random rotation sampler
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER5, false );
			pShaderShadow->EnableTexture( SHADER_SAMPLER6, true );	// flashlight cookie
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER6, false );
			pShaderShadow->EnableTexture( SHADER_SAMPLER7, true ); // lightmap
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER7, g_pHardwareConfig->GetHDRType() == HDR_TYPE_NONE );
			pShaderShadow->EnableTexture( SHADER_SAMPLER8, true ); // BRDF integration LUT
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER8, false );
			pShaderShadow->EnableTexture( SHADER_SAMPLER10, true ); // MRAO LUT
			pShaderShadow->EnableSRGBRead( SHADER_SAMPLER10, false );

			if ( bHasEmissionTexture )
			{
				pShaderShadow->EnableTexture( SHADER_SAMPLER11, true ); // emission texture
				pShaderShadow->EnableSRGBRead( SHADER_SAMPLER11, true );
			}

			if ( bIsWVT )
			{
				pShaderShadow->EnableTexture( SHADER_SAMPLER3, true ); // base texture 2
				pShaderShadow->EnableSRGBRead( SHADER_SAMPLER3, true );
				pShaderShadow->EnableTexture( SHADER_SAMPLER12, true ); // normal map 2
				pShaderShadow->EnableSRGBRead( SHADER_SAMPLER12, true );
				pShaderShadow->EnableTexture( SHADER_SAMPLER13, true ); // MRAO LUT 2
				pShaderShadow->EnableSRGBRead( SHADER_SAMPLER13, false );

				if ( bHasEmissionTexture )
				{
					pShaderShadow->EnableTexture( SHADER_SAMPLER14, true ); // emission texture 2
					pShaderShadow->EnableSRGBRead( SHADER_SAMPLER14, true );
				}
			}

			// lightmap scale is based on HDR type and does not change during the execution of the program
			s_flLightmapScale = pShaderShadow->GetLightMapScaleFactor();

			PI_BeginCommandBuffer();

			// Send ambient cube to the pixel shader, force to black if not available
			PI_SetPixelShaderAmbientLightCube( PSREG_AMBIENT_CUBE );

			// Send lighting array to the pixel shader
			PI_SetPixelShaderLocalLighting( PSREG_LIGHT_INFO_ARRAY );

			// Set up shader modulation color
			PI_SetModulationPixelShaderDynamicState_LinearColorSpace( PSREG_DIFFUSE_MODULATION );

			PI_EndCommandBuffer();
		}
		else // Not snapshotting -- begin dynamic state
		{
			bool bLightingOnly = mat_fullbright.GetInt() == 2 && !IS_FLAG_SET(MATERIAL_VAR_NO_DEBUG_OVERRIDE);

			// Setting up albedo texture
			if (bHasBaseTexture)
			{
				BindTexture(SAMPLER_BASETEXTURE, BASETEXTURE, FRAME);
			}
			else
			{
				pShaderAPI->BindStandardTexture(SAMPLER_BASETEXTURE, TEXTURE_GREY);
			}

			if ( bHasParallaxCorrection )
			{
				pShaderAPI->SetPixelShaderConstant( 3, params[ENVMAPORIGIN]->GetVecValue() );

				const float *vecs[3];
				vecs[0] = params[ENVMAPPARALLAXOBB1]->GetVecValue();
				vecs[1] = params[ENVMAPPARALLAXOBB2]->GetVecValue();
				vecs[2] = params[ENVMAPPARALLAXOBB3]->GetVecValue();
				float matrix[4][4];
				for ( int i = 0; i < 3; i++ )
				{
					for ( int j = 0; j < 4; j++ )
					{
						matrix[i][j] = vecs[i][j];
					}
				}
				matrix[3][0] = matrix[3][1] = matrix[3][2] = 0;
				matrix[3][3] = 1;
				pShaderAPI->SetPixelShaderConstant( 26, &matrix[0][0], 4 );
			}

			// Setting up basecolor tint
			Vector4D vecHsvParams( 1.0f, 1.0f, 1.0f, 0.0f );
			if (bHasHSV)
			{
				params[HSV]->GetVecValue( vecHsvParams.Base(), 3 );
			}
			// Parallax Depth (the strength of the effect)
			vecHsvParams.w = GetFloatParam( PARALLAXDEPTH, params, 3.0f );

			pShaderAPI->SetPixelShaderConstant( 31, vecHsvParams.Base() );

			// Setting up mrao scale
			if ( g_pHardwareConfig->HasFastVertexTextures() )
			{
				Vector4D vecMraoScale( 1.0f, 1.0f, 1.0f, 0.0f );
				if ( bHasMraoScale )
				{
					params[MRAOSCALE]->GetVecValue( vecMraoScale.Base(), 3 );
				}
				vecMraoScale.w = s_flLightmapScale;
				pShaderAPI->SetPixelShaderConstant( 33, vecMraoScale.Base() );
			}

			// Setting up emission scale
			Vector4D vecEmissionScale( 1.0f, 1.0f, 1.0f, 0.0f );
			if (bHasEmissionScale)
			{
				params[EMISSIONSCALE]->GetVecValue( vecEmissionScale.Base(), 3 );
			}
			vecEmissionScale.w = mat_pbr_parallax_dither_amount.GetFloat();
			pShaderAPI->SetPixelShaderConstant( PSREG_SELFILLUMTINT, vecEmissionScale.Base() );

			// Setting up environment map
			if (bHasEnvTexture)
			{
				BindTexture(SAMPLER_ENVMAP, ENVMAP, ENVMAPFRAME);
			}
			else
			{
				pShaderAPI->BindStandardTexture(SAMPLER_ENVMAP, TEXTURE_BLACK);
			}

			// Setting up emissive texture
			if (bHasEmissionTexture)
			{
				BindTexture(SAMPLER_EMISSIVE, EMISSIONTEXTURE, EMISSIONFRAME);
			}
			else
			{
				pShaderAPI->BindStandardTexture(SAMPLER_EMISSIVE, TEXTURE_BLACK);
			}

			// Setting up normal map
			if (bHasNormalTexture)
			{
				BindTexture(SAMPLER_NORMAL, BUMPMAP, BUMPFRAME);
			}
			else
			{
				pShaderAPI->BindStandardTexture(SAMPLER_NORMAL, TEXTURE_NORMALMAP_FLAT);
			}

			// Setting up mrao map
			if (bHasMraoTexture)
			{
				BindTexture(SAMPLER_MRAO, MRAOTEXTURE, MRAOFRAME);
			}
			else
			{
				pShaderAPI->BindStandardTexture(SAMPLER_MRAO, TEXTURE_WHITE);
			}

			if (bHasEmissionFresnel)
			{
				pShaderAPI->SetPixelShaderConstant( 35, params[EMISSIONFRESNEL]->GetVecValue() );
			}

			Vector4D vecMrao2Scale( 1.0f, 1.0f, 1.0f, 0.0f );
			if (bIsWVT)
			{
				BindTexture(SAMPLER_BASETEXTURE2, BASETEXTURE2, FRAME2);

				if (bHasEmissionTexture2)
					BindTexture(SAMPLER_EMISSIVE2, EMISSIONTEXTURE2, EMISSIONFRAME2);
				else
					pShaderAPI->BindStandardTexture(SAMPLER_EMISSIVE2, TEXTURE_BLACK);

				if (bHasNormalTexture2)
					BindTexture(SAMPLER_NORMAL2, BUMPMAP2, BUMPFRAME2);
				else
					pShaderAPI->BindStandardTexture(SAMPLER_NORMAL2, TEXTURE_NORMALMAP_FLAT);

				if (bHasMraoTexture2)
					BindTexture(SAMPLER_MRAO2, MRAOTEXTURE2, MRAOFRAME2);
				else
					pShaderAPI->BindStandardTexture(SAMPLER_MRAO2, TEXTURE_WHITE);

				if (bHasMraoScale2)
					params[MRAOSCALE2]->GetVecValue(vecMrao2Scale.Base(), 3);

				if ( g_pHardwareConfig->HasFastVertexTextures() )
				{
					Vector4D vecEmissionScale2(1.0f, 1.0f, 1.0f, 0.0f);
					if (bHasEmissionScale2)
						params[EMISSIONSCALE2]->GetVecValue(vecEmissionScale2.Base(), 3);
					pShaderAPI->SetPixelShaderConstant( 36, vecEmissionScale2.Base() );
				}
			}
			if ( g_pHardwareConfig->HasFastVertexTextures() )
			{
				// Parallax Center (the height at which it's not moved)
				vecMrao2Scale.w = GetFloatParam( PARALLAXCENTER, params, 3.0f );

				pShaderAPI->SetPixelShaderConstant( 34, vecMrao2Scale.Base() );
			}

			// Getting the light state
			LightState_t lightState;
			pShaderAPI->GetDX9LightState(&lightState);

			// Brushes don't need ambient cubes or dynamic lights
			if (!IS_FLAG_SET(MATERIAL_VAR_MODEL))
			{
				lightState.m_bAmbientLight = false;
				lightState.m_nNumLights = 0;
			}

			// Setting up the flashlight related textures and variables
			bool bFlashlightShadows = false;
			if (bHasFlashlight)
			{
				VMatrix worldToTexture;
				ITexture *pFlashlightDepthTexture;
				FlashlightState_t state = pShaderAPI->GetFlashlightStateEx( worldToTexture, &pFlashlightDepthTexture );
				BindTexture( SAMPLER_FLASHLIGHT, state.m_pSpotlightTexture, state.m_nSpotlightTextureFrame );
				bFlashlightShadows = state.m_bEnableShadows && (pFlashlightDepthTexture != NULL);

				SetFlashLightColorFromState(state, pShaderAPI, PSREG_FLASHLIGHT_COLOR);

				if (pFlashlightDepthTexture && g_pConfig->ShadowDepthTexture() && state.m_bEnableShadows)
				{
					BindTexture(SAMPLER_SHADOWDEPTH, pFlashlightDepthTexture, 0);
					pShaderAPI->BindStandardTexture(SAMPLER_RANDOMROTATION, TEXTURE_SHADOW_NOISE_2D);
				}
			}

			// Getting fog info
			MaterialFogMode_t fogType = pShaderAPI->GetSceneFogMode();
			int fogIndex = (fogType == MATERIAL_FOG_LINEAR_BELOW_FOG_Z) ? 1 : 0;

			// Getting skinning info
			int numBones = pShaderAPI->GetCurrentNumBones();

			// Some debugging stuff
			bool bWriteDepthToAlpha = false;
			if (bFullyOpaque)
			{
				bWriteDepthToAlpha = pShaderAPI->ShouldWriteDepthToDestAlpha();
			}

			// Determining the max level of detail for the envmap
			int iEnvMapLOD = 6;
			if (bHasEnvTexture)
			{
				int width = 0;
				// Get power of 2 of texture width
				if (ITexture *envTexture = params[ENVMAP]->GetTextureValue())
					width = envTexture->GetMappingWidth();
				Assert( width != 0 );
				int mips = 0;
				while (width >>= 1)
					++mips;

				// Cubemap has 4 sides so 2 mips less
				iEnvMapLOD = mips;
			}

			// Dealing with very high and low resolution cubemaps
			if (iEnvMapLOD > 12)
				iEnvMapLOD = 12;
			if (iEnvMapLOD < 4)
				iEnvMapLOD = 4;

			// This has some spare space
			float vEyePos_SpecExponent[4];
			pShaderAPI->GetWorldSpaceCameraPosition( vEyePos_SpecExponent );
			vEyePos_SpecExponent[3] = iEnvMapLOD;
			pShaderAPI->SetPixelShaderConstant(PSREG_EYEPOS_SPEC_EXPONENT, vEyePos_SpecExponent, 1);

			// Setting lightmap texture
			if (bLightMapped)
				s_pShaderAPI->BindStandardTexture(SAMPLER_LIGHTMAP, TEXTURE_LIGHTMAP_BUMPED);

			BindTexture(SAMPLER_BRDF_INTEGRATION, BRDF_INTEGRATION);

			if ( !g_pHardwareConfig->HasFastVertexTextures() || mat_pbr_force_20b.GetBool() )
			{
				// Setting up dynamic vertex shader
				DECLARE_DYNAMIC_VERTEX_SHADER( pbr_vs20 );
				SET_DYNAMIC_VERTEX_SHADER_COMBO(DOWATERFOG, fogIndex);
				SET_DYNAMIC_VERTEX_SHADER_COMBO(SKINNING, numBones > 0);
				SET_DYNAMIC_VERTEX_SHADER_COMBO(LIGHTING_PREVIEW, pShaderAPI->GetIntRenderingParameter(INT_RENDERPARM_ENABLE_FIXED_LIGHTING) != 0);
				SET_DYNAMIC_VERTEX_SHADER_COMBO(COMPRESSED_VERTS, ( int )vertexCompression);
				SET_DYNAMIC_VERTEX_SHADER_COMBO(NUM_LIGHTS, lightState.m_nNumLights);
				SET_DYNAMIC_VERTEX_SHADER(pbr_vs20);

				// Setting up dynamic pixel shader
				DECLARE_DYNAMIC_PIXEL_SHADER(pbr_ps20b);
				SET_DYNAMIC_PIXEL_SHADER_COMBO(NUM_LIGHTS, lightState.m_nNumLights);
				SET_DYNAMIC_PIXEL_SHADER_COMBO(WRITE_DEPTH_TO_DESTALPHA, bWriteDepthToAlpha);
				SET_DYNAMIC_PIXEL_SHADER_COMBO(FLASHLIGHTSHADOWS, bFlashlightShadows);
				SET_DYNAMIC_PIXEL_SHADER(pbr_ps20b);
			}
			else
			{
				// Setting up dynamic vertex shader
				DECLARE_DYNAMIC_VERTEX_SHADER(pbr_vs30);
				SET_DYNAMIC_VERTEX_SHADER_COMBO(DOWATERFOG, fogIndex);
				SET_DYNAMIC_VERTEX_SHADER_COMBO(SKINNING, numBones > 0);
				SET_DYNAMIC_VERTEX_SHADER_COMBO(LIGHTING_PREVIEW, pShaderAPI->GetIntRenderingParameter(INT_RENDERPARM_ENABLE_FIXED_LIGHTING) != 0);
				SET_DYNAMIC_VERTEX_SHADER_COMBO(COMPRESSED_VERTS, ( int )vertexCompression);
				SET_DYNAMIC_VERTEX_SHADER_COMBO(NUM_LIGHTS, lightState.m_nNumLights);
				SET_DYNAMIC_VERTEX_SHADER(pbr_vs30);

				// Setting up dynamic pixel shader
				DECLARE_DYNAMIC_PIXEL_SHADER(pbr_ps30);
				SET_DYNAMIC_PIXEL_SHADER_COMBO(NUM_LIGHTS, lightState.m_nNumLights);
				SET_DYNAMIC_PIXEL_SHADER_COMBO(WRITE_DEPTH_TO_DESTALPHA, bWriteDepthToAlpha);
				SET_DYNAMIC_PIXEL_SHADER_COMBO(FLASHLIGHTSHADOWS, bFlashlightShadows);
				SET_DYNAMIC_PIXEL_SHADER(pbr_ps30);
			}

			// Handle mat_fullbright 2 (diffuse lighting only)
			if (bLightingOnly)
			{
				pShaderAPI->BindStandardTexture(SAMPLER_BASETEXTURE, TEXTURE_GREY); // Basecolor
			}

			if ( useParallax )
			{
				int nSamples[4]{};
				nSamples[0]  = PARALLAX_SAMPLES[clamp(mat_pbr_parallaxmap_quality.GetInt(), 0, useParallax ? PARALLAX_QUALITY_MAX : 0)];

				// Apply optional scale to parallax sample count. Capping at 256 to avoid stupid materials
				if ( params[PARALLAXSCALE]->IsDefined() )
					nSamples[0] = clamp( GetFloatParam( PARALLAXSCALE, params, 1.0f ) * nSamples[0], 0, 256 );

				pShaderAPI->SetIntegerPixelShaderConstant( 32, nSamples );
			}

			SetVertexShaderTextureTransform( VERTEX_SHADER_SHADER_SPECIFIC_CONST_0, BASETEXTURETRANSFORM );
		}

		// Actually draw the shader
		Draw();
	};

// Closing it off
END_SHADER;
