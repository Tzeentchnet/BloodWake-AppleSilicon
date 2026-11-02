# Metal quality settings

## Adjustable LOD

The native Apple Silicon build uses the same Remastered draw-distance hook as
upstream. Original remains fixed at the console's 1x behavior. Remastered can
select 1x through 8x, and the active value is now an atomic runtime setting:
the visibility hook can read a new value without rebuilding game state or
mutating the edition registry.

## Anisotropic filtering

Metal now prebuilds immutable 1x, 2x, 4x, 8x, and 16x sampler variants for the
existing address/filter/mip combinations. The selected anisotropy changes at
an executor flip boundary. It is applied only when a texture has multiple mip
levels and uses linear minification. Point-filtered and unmipped textures,
including UI paths, remain 1x.

Invalid `BW_VFX_AF` values stop Metal startup with an explicit error. Original
ignores the Remastered setting and stays at 1x. The settings UI exposes the
existing Off, 4x, 8x, and 16x choices.

## Metal MSAA decision

The DX11 `BW_VFX_MSAA` setting is not implemented by the Metal backend and is
reported as unsupported rather than silently accepted.

At the currently verified 2560x1920 internal size, one BGRA8 colour target is
18.75 MiB and one Z24S8 depth/stencil target is another 18.75 MiB. Keeping the
authoritative single-sample pair while adding multisample colour and depth has
these lower-bound attachment costs:

| Sample count | Multisample pair added | Single + multisample pairs |
| --- | ---: | ---: |
| 2x | 75 MiB | 112.5 MiB |
| 4x | 150 MiB | 187.5 MiB |
| 8x | 300 MiB | 337.5 MiB |

These figures exclude presentation, seed/readback resources, cached inactive
targets, textures, resolve traffic, and driver allocation overhead. MSAA would
also require sample-count pipeline variants, persistent multisample colour and
depth ownership, resolves before sampling/readback/presentation, and correct
handling of target switches, partial clears, guest-seeded surfaces, resize,
render-scale changes, and quincunx.

The project therefore has a current **no-go** on advertising or implementing
Metal MSAA without a controlled retail A/B run showing a visible edge-quality
benefit beyond native render scaling at an acceptable GPU-time and memory
cost. A future evaluation must record monotonic frame-time percentiles, GPU
duration where available, peak attachment allocation, resolve cost, and fixed
reference crops containing hulls, rigging, shorelines, particles, HUD, and
alpha-tested edges at 1x, 2x, and 4x render scale. If that evidence does not
justify multisampling, native render scale and the validated guest AA path
remain the Metal alternatives.
