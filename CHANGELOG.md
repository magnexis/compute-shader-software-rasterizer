# Changelog

## [1.0.1] - 2026-06-13

### Fixed
- Fixed D3D12 descriptor heap leak caused by un-freed SRV/UAV descriptors between frames
- Descriptor allocation now correctly releases resources on pipeline teardown and window resize