#ifndef ALPHAMODE_H
#define ALPHAMODE_H

const int alpha_mode_premult = 0;
const int alpha_mode_coverage = 1;
const int alpha_mode_none = 2;
const int alpha_mode_invert = 3;

const int alpha_mode_max_bits = 4;

uint get_layer_alphamode(uint layerIdx) {
    return bitfieldExtract(u_alphaMode, int(layerIdx) * alpha_mode_max_bits, alpha_mode_max_bits);
}

vec4 BlendLayer( uint layerIdx, vec4 outputValue, vec4 layerColor, float opacity )
{
    float layerAlpha = opacity * layerColor.a;
    
    uint alphaMode = get_layer_alphamode( layerIdx );
    if ( alphaMode == alpha_mode_premult )
    {
        // wl_surfaces come with premultiplied alpha, so that's them being
        // premultiplied by layerColor.a.
        // We need to then multiply that by the layer's opacity to get to our
        // final premultiplied state.
        // For the other side of things, we need to multiply by (1.0f - (layerColor.a * opacity))
        outputValue = layerColor * opacity + outputValue * (1.0f - layerAlpha);
    }
    else if ( alphaMode == alpha_mode_coverage ) // coverage for accessibility looks
    {
        outputValue = layerColor * layerAlpha + outputValue * (1.0f - layerAlpha);
    }
    else if ( alphaMode == alpha_mode_invert )
    {
        // True per-pixel "invert what's underneath" mode -- the FPS HUD's
        // Inverted text-colour option (superdoc/features/fps-display.md).
        //
        // The layer carries BOTH kinds of content at once and this mode
        // tells them apart by the layer's own brightness (see the
        // selector below): near-white texels invert the destination,
        // everything darker composites normally, exactly like
        // alpha_mode_coverage. That is what lets the HUD keep its
        // backdrop, its black outline and its inverted digits in ONE
        // layer.
        //
        // Why one layer and not two: an earlier attempt put the backdrop
        // and the outline in a separate, normally blended layer BELOW
        // this one. That layer then painted over the game everywhere the
        // digits were about to land, so this mode inverted the HUD's own
        // backdrop/outline instead of the game and the digits came out a
        // constant near-white -- "inverted mode stopped inverting". A
        // destination-reading blend cannot be split across two layers
        // that overlap; keep it in one.

        // Nothing to do where this layer is transparent -- the blend below
        // would return outputValue bit-for-bit anyway, and this skips the
        // transfer-function maths for every pixel the HUD does not cover.
        if ( layerAlpha <= 0.0f )
            return outputValue;

        // This runs after apply_layer_color_mgmt() and before
        // encodeOutputColor(), so `outputValue` is a LINEAR-light
        // blend-space colour that, under HDR/PQ, is not bounded to [0,1] --
        // clamp before inverting, or an HDR background can push the
        // inverted result negative/out-of-range.
        vec3 bg = clamp( outputValue.rgb, 0.0f, 1.0f );
        vec3 inverted = 1.0f - bg;

        // Contrast guard, judged in ENCODED (sRGB) space. A literal invert
        // is 1 - bg in linear light, but the eye judges the gap between
        // the digit and the background on the encoded (gamma) scale, and
        // the two disagree badly in the mid-tones: a background of
        // encoded 148 is linear 0.30, its true invert is linear 0.70,
        // and that encodes to ~218 -- a faint light grey over mid grey
        // that reads as "white text ignoring the background". The
        // earlier guard measured the gap in linear light (floor 0.40,
        // engaging only for linear luma in (0.30, 0.70)) and so left
        // exactly those backgrounds alone. Encoded luma (Rec.709
        // weights on the encoded channels, i.e. Y') is the quantity the
        // eye actually compares, so measure there and push there.
        //
        // The rule: if |Y'(inverted) - Y'(bg)| < kMinEncodedSeparation,
        // move the inverted colour uniformly (all channels, so whatever
        // hue survives is kept) to sit exactly that far from the
        // background's Y' on the side AWAY from the background --
        // below it for backgrounds brighter than perceptual mid grey
        // (Y' > 0.5), above it for darker ones. Where the true invert
        // already clears the floor it is left exactly as it is.
        //
        // Why 0.40: at Y'(bg) == 0.5 the true invert's Y' is ~0.896, a
        // gap of ~0.40, so this floor is precisely "what a true invert
        // gives you at perceptual mid grey" -- the true inversion
        // survives untouched for EVERY background darker than encoded
        // 128, and the push only ever engages on the bright side
        // (encoded ~128..229 for greys), where it pulls the digit down
        // to bg - 0.40 (encoded 148 -> ~46, 188 -> ~86, 219 -> ~117).
        // See superdoc/features/fps-display.md's expected table.
        const vec3 kLumaWeights = vec3( 0.2126f, 0.7152f, 0.0722f );
        const float kMinEncodedSeparation = 0.40f;
        vec3 invertedEnc = linearToSrgb( inverted );
        float flBgLumaEnc = dot( linearToSrgb( bg ), kLumaWeights );
        float flInvLumaEnc = dot( invertedEnc, kLumaWeights );
        if ( abs( flInvLumaEnc - flBgLumaEnc ) < kMinEncodedSeparation )
        {
            float flTargetLumaEnc = flBgLumaEnc > 0.5f
                ? flBgLumaEnc - kMinEncodedSeparation
                : flBgLumaEnc + kMinEncodedSeparation;
            invertedEnc = clamp( invertedEnc + ( flTargetLumaEnc - flInvLumaEnc ), 0.0f, 1.0f );
            inverted = srgbToLinear( invertedEnc );
        }

        // The selector: how much of THIS texel wants the inverted colour
        // rather than its own. The HUD draws the digits' fill in pure
        // opaque white and everything that must not invert (the backdrop,
        // the black outline) far darker, so the layer's own luma
        // separates them with room to spare -- white is 1.0 in this
        // linear-light space, the darkest usable backdrop tint well under
        // 0.25. Anti-aliased glyph edges land in between and cross-fade,
        // which is exactly the right thing for them to do.
        float flLayerLuma = dot( clamp( layerColor.rgb, 0.0f, 1.0f ), kLumaWeights );
        float flInvertSelect = smoothstep( 0.25f, 0.80f, flLayerLuma );
        vec3 target = mix( layerColor.rgb, inverted, flInvertSelect );

        // Gate on this layer's own alpha so only covered pixels are
        // touched at all; fully transparent pixels pass the background
        // through unchanged. With flInvertSelect == 0 this is bit-for-bit
        // the alpha_mode_coverage blend above.
        outputValue.rgb = mix( outputValue.rgb, target, layerAlpha );
        outputValue.a = layerAlpha + outputValue.a * ( 1.0f - layerAlpha );
    }
    else // none
    {
        outputValue = layerColor * opacity;
    }

    return outputValue;
}

vec3 BlendLayer( uint layerIdx, vec3 outputValue, vec4 layerColor, float opacity )
{
    return BlendLayer( layerIdx, vec4( outputValue, 1 ), layerColor, opacity ).rgb;
}

#endif