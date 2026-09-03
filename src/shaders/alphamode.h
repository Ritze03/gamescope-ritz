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
        // This runs after apply_layer_color_mgmt() and before
        // encodeOutputColor(), so `outputValue` is a LINEAR-light
        // blend-space colour that, under HDR/PQ, is not bounded to [0,1] --
        // clamp before inverting, or an HDR background can push the
        // inverted result negative/out-of-range.
        vec3 bg = clamp( outputValue.rgb, 0.0f, 1.0f );
        vec3 inverted = 1.0f - bg;

        // Rec.709 luma weights -- same ones FpsDisplay.cpp's host-side code
        // uses. They sum to 1.0, so a literal invert's luma is exactly
        // (1.0 - bgLuma): that collapses to zero separation from the
        // background right at bgLuma == 0.5, making inverted text vanish
        // over mid-grey. Push the inverted colour toward black or white,
        // just far enough to clear a minimum luma gap, but ONLY when the
        // true invert doesn't already clear it on its own -- everywhere
        // else the real inversion survives untouched.
        const vec3 kLumaWeights = vec3( 0.2126f, 0.7152f, 0.0722f );
        float flBgLuma = dot( bg, kLumaWeights );
        float flInvLuma = 1.0f - flBgLuma;
        float flSeparation = abs( flInvLuma - flBgLuma );

        // Chosen from a measured on-screen check, not a formal contrast
        // standard: at background RGB(188,188,188) (encoded), a literal
        // invert reads legible-but-marginal at 0.25f (inverted text lands
        // at ~RGB(138,138,138), grey-on-grey). Raised to 0.40f so that same
        // background reads at ~RGB(91,91,91) -- comfortably readable.
        // Raising this further keeps buying legibility in the mid band at
        // the cost of how much of the *true* inverted colour survives
        // there -- that trade is the deliberate tension in this knob.
        const float kMinLumaSeparation = 0.40f;
        if ( flSeparation < kMinLumaSeparation )
        {
            float flPush = kMinLumaSeparation - flSeparation;
            float flDir = flBgLuma > 0.5f ? -1.0f : 1.0f; // away from the background's own luma
            inverted = clamp( inverted + flDir * flPush, 0.0f, 1.0f );
        }

        // Gate on this layer's own alpha so only glyph pixels (near-opaque
        // in the HUD's offscreen texture) actually get inverted; fully
        // transparent pixels pass the background through unchanged.
        outputValue.rgb = mix( outputValue.rgb, inverted, layerAlpha );
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