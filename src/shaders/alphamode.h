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

// The FPS HUD's single-sample Inverted colour mode (REDESIGNED 2026-09-27,
// superdoc/features/fps-display.md's Inverted section). The whole digit is
// coloured with ONE sample of layer 0 -- taken at the readout box's own
// centre, inverted -- rather than each glyph pixel inverting the real
// composited colour underneath it (the previous, true per-pixel invert).
// Set once per invocation by the calling .comp file, from its own layer-0
// sampling convention (each composite path reads layer 0 differently --
// see each file's own comment next to where it assigns this), before the
// layer loop that calls BlendLayer() runs; alpha_mode_invert below just
// reads it. A plain global rather than a BlendLayer() parameter: several
// of these files already call BlendLayer() from more than one place (both
// blur passes loop over every layer twice under different conditions), and
// threading a new argument through every call site is more invasive than
// setting one shared value at the top of main().
//
// Why single-sample and not per-pixel: the user's own report was "the
// inverted color mode ... doesn't work at the moment, it just stays
// white" -- and separately, once told the code already inverted the real
// pixel underneath, asked for exactly this instead: "It should capture a
// single pixel of the color below it and just invert it and use that
// color so it constantly changes. it's more like an OLED thingy." Code
// review of the old per-pixel path (superdoc/features/fps-display.md's
// "Root cause" note) found the maths itself intact and unchanged since its
// 2026-09-09 fix, but also a genuine bug in cs_composite_blur_cond.comp's
// ordering (BLUR_MODE_COND blends every layer at or above
// c_blur_layer_count -- where the HUD always sits -- BEFORE layer 0 is
// blended in, so the old code's "invert the destination" read pure black
// there and produced a constant white independent of the game or of any
// Shaders toggle); a locally-uniform patch of game content under the
// readout (a dark HUD corner, say) would look the same way even where the
// maths was correct, since a per-pixel invert of a uniform patch IS a
// uniform colour. This redesign sidesteps both: it never reads the
// composited destination at all, so neither cause can reproduce it, and
// the one sample it does take changes with the frame instead of holding
// steady over a static patch of chrome.
vec3 g_hudInvertColor = vec3(0.0);

// CORRECTED 2026-09-27 (same day as the redesign above): the sample must be
// inverted in ENCODED (sRGB-ish, "what the pixel looks like on screen")
// space, not linear light. A plain `1.0 - linearColor` is a LINEAR invert,
// and for the dark-to-mid scenes that dominate real gameplay (CS2 included)
// that lands very close to white: encoded 51 -> linear ~0.03 -> 1-0.03 =
// 0.97 -> re-encoded ~251; encoded 148 -> ~218. Verified live (this file's
// own measurements): every one of three test backgrounds inverted to
// within a few counts of white or near-white under the first cut of this
// redesign, reproducing the exact "it just stays white" complaint the
// whole redesign exists to fix -- just for a different reason than the
// per-pixel invert's bugs (see the Root Cause note above). The user's own
// words were "just invert it ... it's more like an OLED thingy", which
// means the plain, everyday sense of "invert a pixel": what you'd get from
// `255 - x` on the number actually on screen. That IS what a naive
// `1.0 - c` gives for encoded values, so encode the linear sample first,
// invert THAT, then decode back to linear for BlendLayer's own blend
// space -- for SDR output the final composited channel value is exactly
// `255 - bg` (encoded 51 -> 204, 148 -> 107, verified against
// superdoc/features/fps-display.md's own measured table).
vec3 InvertEncodedSample( vec3 linearColor )
{
    vec3 encoded = linearToSrgb( linearColor );
    return srgbToLinear( clamp( 1.0f - encoded, 0.0f, 1.0f ) );
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
        // The FPS HUD's single-sample Inverted text-colour option
        // (superdoc/features/fps-display.md) -- see g_hudInvertColor's own
        // comment above for the technique and why it replaced a true
        // per-pixel invert of the destination.
        //
        // The layer carries BOTH kinds of content at once: texels that
        // must take g_hudInvertColor (the digits' fill) and texels that
        // must composite normally (the HUD's black outline, and the
        // crosshair in whatever colour the user picked). They are
        // told apart by a MARKER the HUD encodes into the texel itself:
        //
        //   the digits are drawn in pure magenta, (1, 0, 1), and every
        //   other HUD element that can sit under a digit's antialiased
        //   edge (the outline and the clear colour -- and, until
        //   2026-09-09, the backdrop) is pure
        //   black -- so after ImGui's straight-alpha blend a texel with
        //   G == 0 is exactly  d * (1,0,1) + (a - d) * black,  where d is
        //   the digit coverage and a the texel's alpha. G == 0 therefore
        //   marks "digit (plus black)", R recovers d, and (a - d) is how
        //   much black outline shows around it: the shader
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

            // No contrast guard, on purpose: the user asked for a plain
            // invert of one pixel ("It should capture a single pixel of
            // the color below it and just invert it and use that color"),
            // and a mid-grey sample inverting to mid-grey is the honest
            // OLED-style behaviour that was asked for, not a defect to
            // guard against -- see g_hudInvertColor's own comment for the
            // full "why". g_hudInvertColor is already clamped to [0,1]
            // before inverting by whichever .comp file set it (the HDR/PQ
            // caveat the old per-pixel invert had here applies equally to
            // that one sample).
            //
            // Digit share takes g_hudInvertColor; the remaining covered
            // share is black (the outline) and contributes nothing; the
            // rest of the pixel is the background, untouched. A pure-black
            // texel (outline only, d == 0) is therefore exactly the
            // coverage blend of black, and a full digit texel (d == a == 1)
            // is exactly g_hudInvertColor.
            outputValue.rgb = outputValue.rgb * ( 1.0f - layerAlpha ) + g_hudInvertColor * flDigit;
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