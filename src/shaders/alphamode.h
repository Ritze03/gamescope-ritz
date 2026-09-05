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
        // The layer carries BOTH kinds of content at once: texels that
        // must invert the destination (the digits' fill) and texels that
        // must composite normally (the HUD's backdrop and black outline,
        // and the crosshair in whatever colour the user picked). They are
        // told apart by a MARKER the HUD encodes into the texel itself:
        //
        //   the digits are drawn in pure magenta, (1, 0, 1), and every
        //   other HUD element that can sit under a digit's antialiased
        //   edge (the outline, the backdrop, the clear colour) is pure
        //   black -- so after ImGui's straight-alpha blend a texel with
        //   G == 0 is exactly  d * (1,0,1) + (a - d) * black,  where d is
        //   the digit coverage and a the texel's alpha. G == 0 therefore
        //   marks "digit (plus black)", R recovers d, and (a - d) is how
        //   much black outline/backdrop shows around it: the shader
        //   reconstructs the layering exactly. Anything with G > 0 is not
        //   a digit and blends bit-for-bit as alpha_mode_coverage would.
        //   The crosshair keeps every colour it can be given at any
        //   opacity, since the HUD nudges a crosshair colour with G == 0 to
        //   G == 1 (a one-count change) and renders the shared texture at
        //   16 bits per channel, so even a 1/255 green at low opacity is
        //   still a non-zero texel after premultiplication.
        //
        // Why a marker and not the layer's brightness (the 2026-09-03 to
        // 2026-09-06 rule, smoothstep on the texel's luma): brightness
        // cannot tell a white digit from a white crosshair, so the
        // crosshair had to leave this layer -- a second Layer_t sampling
        // the bottom half of a double-height texture -- and that cost one
        // of the six layer slots. Why not a two-layer split in general: a
        // blend that reads the destination cannot sit above another layer
        // covering the same pixels (the 2026-09-03 breakage, where the
        // lower layer's backdrop became the "background" the digits
        // inverted).

        // Nothing to do where this layer is transparent -- the blend below
        // would return outputValue bit-for-bit anyway, and this skips the
        // transfer-function maths for every pixel the HUD does not cover.
        if ( layerAlpha <= 0.0f )
            return outputValue;

        // sampleRegular() has already decoded the texel's RGB from sRGB
        // to linear; alpha is raw. A zero survives the decode exactly
        // (the linear segment), so the marker test is an exact compare.
        if ( layerColor.g <= 0.0f )
        {
            // Digit coverage, in the ENCODED domain ImGui blended in: the
            // stored R is d * 1 + (1 - d) * 0 for a digit over black or
            // clear, i.e. d itself, so re-encode the decoded R to get it
            // back. Never more than the texel's alpha (rounding guard).
            float flDigit = min( linearToSrgb( vec3( layerColor.r ) ).r, layerAlpha );

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

            // Digit share inverts; the remaining covered share is black
            // (outline / backdrop) and contributes nothing; the rest of the
            // pixel is the background, untouched. A pure-black texel
            // (outline only, d == 0) is therefore exactly the coverage
            // blend of black, and a full digit texel (d == a == 1) is
            // exactly the inverted colour.
            outputValue.rgb = outputValue.rgb * ( 1.0f - layerAlpha ) + inverted * flDigit;
        }
        else
        {
            // Not a digit: bit-for-bit the alpha_mode_coverage blend above.
            outputValue.rgb = layerColor.rgb * layerAlpha + outputValue.rgb * ( 1.0f - layerAlpha );
        }
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