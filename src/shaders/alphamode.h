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
        //
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