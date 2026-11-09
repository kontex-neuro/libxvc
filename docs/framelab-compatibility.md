# ThorVision FrameLab naming compatibility

The research workflow is named **ThorVision FrameLab**. Feature work uses the
`feat/framelab` branch across the client, shared library and videoserver.

This branding migration preserves `xvc::analytics`, its public header and recorded
`.analytics` sidecars, schema IDs and bundle formats. Existing recordings and
SDK callbacks remain readable; no C++ ABI or scientific data migration is needed.
The client prefers the `/framelab/v1` device gateway and supports the legacy
`/analytics/v1` gateway through read-only discovery. Mutating requests must never
be replayed against an alternate prefix after an ambiguous failure.
