# Patch engine provenance

This directory adapts the non-UI engine supplied in
[DrKet/ShadowPlay_Patcher-2.0](https://github.com/DrKet/ShadowPlay_Patcher-2.0),
which builds on
[furyzenblade/ShadowPlay_Patcher](https://github.com/furyzenblade/ShadowPlay_Patcher).
Credit for the original research and export-hook approach belongs to the original authors.

AlwaysShadow integrates the strategy/manager, remote hooks, signature scanner,
target selection and external configuration. Its own worker, tray and startup
handling replace the standalone CLI, GUI and watch loop.

Local changes include conservative signature matching, validation of configuration
and original code, session-scoped targeting, process identity checks, reversible
worker lifecycle and truthful undo failures. Configuration uses the cJSON library
already included by AlwaysShadow, with embedded defaults, current-user registry
storage and a one-time import of existing configuration files.
