# Native NSP Packaging and Internet Self-Updates

## What this project builds

The project keeps a development `.nro` and builds a native, HOME Menu application `.nsp` from the same compiled program. The NSP does not rename or wrap the NRO: its Program NCA contains a native NSO executable and NPDM process metadata.

Install a release containing the native updater once through an installer such as DBI. Subsequent compatible releases can be downloaded, validated, installed, and restarted from inside the application. Downloading a file alone is not an installed-title update; the application must register its new contents and metadata with Horizon.

This is an unsigned homebrew application workflow for a homebrew-capable Switch environment that permits unsigned content. It is not Nintendo's official retail distribution mechanism. A fake signature is expected in that environment, but does not establish that a package is structurally valid.

## Package structure

```text
Application.nsp                  PFS0 container
├── <content-id>.nca              Program
│   ├── ExeFS/main               Native NSO executable
│   ├── ExeFS/main.npdm          Process identity and permissions
│   └── RomFS                    Application assets
├── <content-id>.nca              Control
│   ├── control.nacp             Name, publisher, version, account policy
│   └── icon_AmericanEnglish.dat 256×256 JPEG HOME Menu icon
├── <content-id>.nca              Meta
│   └── Application CNMT         Title/version and content references
└── connections-update.json      Project-specific update discovery manifest
```

The manifest is an auxiliary member of this project's NSP, not a substitute for CNMT and not a required component of every NSP. Horizon's application launch contents remain the Program, Control, and Meta NCAs.

## Build tools and prerequisites

The build uses devkitPro/devkitA64 and libnx, plus:

- `elf2nso` to produce the native executable.
- `npdmtool` to generate `main.npdm` from process configuration.
- `nacptool` to create application control metadata.
- [hacPack](https://github.com/DarkMatterCore/hacPack) to generate NCAs and the NSP container.
- [nstool](https://github.com/jakcron/nstool) to inspect/extract generated content.
- Python for packaging orchestration and final validation.
- The project's PowerShell icon conversion script.

Supply an appropriate, legitimately obtained external keyset when required by the tools. Keep keys outside the repository, never print them in logs, and never bundle them with a release. No Nintendo SDK or copyrighted Nintendo icon assets are required by this workflow.

Environment overrides supported by the project:

```powershell
$env:CONNECTIONS_HACPACK = 'C:\tools\hacPack\hacpack.exe'
$env:CONNECTIONS_NSP_KEYS = 'C:\private\prod.keys'
$env:CONNECTIONS_NSTOOL = 'C:\tools\nstool\nstool.exe'
```

These are example paths. The packaging script also has local defaults; set explicit paths when reusing the workflow on another machine.

## Packaging steps

1. Compile the application and preserve its working NRO target. Generate an NSO from the compiled ELF for native packaging.
2. Choose an application title ID that does not collide with another installed application. Keep it consistent in NPDM, NACP title-related fields, NCA headers, CNMT, manifest, and runtime installer validation. Keep the same ID for subsequent releases of the same application. The packager's ID format check does not prove uniqueness against a console's installed titles.
3. Generate `main.npdm` with the correct title ID, application type, stack, thread configuration, filesystem access, services, and kernel capabilities. This project currently uses broad service/filesystem permissions for homebrew operation; they are not a minimal permission template. Narrow them when adapting to another application, while retaining permissions needed by graphics, audio, account access, SD storage, and native installation.
4. Generate `control.nacp` with the application name, publisher, display version, and supported languages. This project uses `Connections`, `Zoboo`, and the version from `source/core/version.h`.
5. Set `StartupUserAccount = Required` when the application needs the selected Switch profile. This project explicitly writes value `1` at NACP offset `0x3025`. Account selection still requires correct runtime Account API use and permissions.
6. Convert the application-owned icon to a valid 256×256 JPEG and put it inside the Control NCA's RomFS under the language-specific icon filename. A build-time NRO icon alone does not provide the installed HOME Menu icon. This project advertises American English and supplies `icon_AmericanEnglish.dat`.
7. Use hacPack to produce the Program NCA from ExeFS/RomFS and the Control NCA from its metadata/icon directory.
8. Generate an application CNMT referencing the generated Program and Control contents with the correct content IDs, types, sizes, hashes, and title version. This project extracts a provisional CNMT, computes its SHA-256 digest, and regenerates Meta through hacPack with that digest because the tool's default digest is zero.
9. Add the small update manifest and use hacPack to produce the NSP. This project uses hacPack's plaintext NCA option; do not assume this configuration is suitable for an unrelated retail packaging workflow.
10. Extract and verify the final NSP before publishing it. Publish only the verified candidate, then remove temporary staging output.

Do not repair a malformed package by renaming an NRO or inventing a replacement NSP/NCA container. Correct the tool inputs and regenerate through the packaging tools.

## Versioning

This project stores two release values in `source/core/version.h`:

```c
#define CONNECTIONS_APP_VERSION "1.1.6"
#define CONNECTIONS_PACKAGE_REVISION 9
```

The display version appears in the application, NACP, and update manifest. Native ordering uses this project-defined integer encoding:

```text
native_version = (major << 24) | (minor << 16) | (patch << 8) | package_revision
```

Each component is limited to `0..255`. The same native integer goes into CNMT and the manifest. It is a project convention, not a universal NSP version formula.

Every published release must have a strictly newer native version. Increase the display version for changes users need to identify, and the package revision for packaging-only changes. Keep the UI, NACP, manifest, and runtime expected version consistent.

This updater replaces the application's base content with a newer application package. It does not generate or install a separate Nintendo-style patch-title NSP. Do not mix these two update schemes without deliberately implementing their different metadata handling.

## GitHub Releases release hosting

The release endpoint is the shared release asset:

[Connections NSP release](https://github.com/Zoboo/Connections/releases/latest)

The runtime uses the download endpoint configured in `platform_updates.c`:

```text
https://github.com/Zoboo/Connections/releases/latest/download/Connections.nsp
```

After building a release, replace the contents of that existing release asset while retaining its file ID and download access. The build does not upload automatically. A new file ID requires an endpoint change in already installed clients unless a separate stable discovery endpoint is introduced.

The version check reads bounded HTTP ranges: first the NSP index, then the small embedded manifest. It checks the application identity and advertised version before offering an update. Full-package downloads happen only after acceptance. The current implementation requires valid partial-content responses for discovery; Drive permission changes, quotas, HTML interstitials, or missing range support can make a check fail.

For another project, replace the endpoint, manifest filename/schema expectations, title identity, version constants, metadata validators, and temporary paths together. The current installer intentionally accepts only this application's packages.

## Runtime update sequence

```text
Launch -> background version check -> newer release available
       -> UPDATE accepted -> download temporary NSP -> validate
       -> install contents -> commit metadata -> update application record
       -> cleanly shut down -> restart installed application
```

Networking, file downloading, hashing, and native installation run on an update worker. The main thread presents the offer and reads atomic progress. It never performs download I/O or waits for a server from the frame loop.

The temporary download is:

```text
sdmc:/switch/Connections/connections-update.nsp.tmp
```

The native installer:

1. Confirms it is running as the installed native application, with the expected program ID, rather than through the NRO loader.
2. Validates the bounded NSP layout, manifest, expected version, NCA content hashes/IDs, CNMT references, and Control NACP identity/version/account policy.
3. Opens Nintendo Content Manager (NCM) storage and metadata services and the Nintendo Shell (NS) application manager interface. These are local Horizon services, not Nintendo online services.
4. Finds the currently installed application's storage and metadata record, rejects non-newer versions, and checks free space.
5. Writes new content through bounded NCM placeholders, verifies hashes while streaming, flushes, and registers completed contents. Already present contents are checked before reuse.
6. Validates CNMT and Control data before committing the new metadata key.
7. Updates the application's NS record to reference the new base metadata, then attempts control-cache invalidation.
8. Removes the temporary download. On success, the main thread finishes normal resource cleanup and calls `appletRestartProgram(NULL, 0)`.

The NSP's NPDM must allow the NCM, NS, filesystem, networking, and process operations actually used by the installer. Runtime permissions do not come from the remote download. This project uses explicit application-manager IPC wrappers for record operations; these are platform-specific and must be checked against the target Horizon/libnx environment when reused.

The progress bar reserves `0..80%` for NSP download, `80..85%` for content verification, and `85..99%` for content installation. It reaches `100%` only after successful installation. Unknown download sizes use an indeterminate animation. Progress crosses threads through atomics, without per-frame disk logging.

## Failure handling and limits

- Failed downloads do not replace the installed application.
- The installer preserves old running contents and metadata while preparing the new release. It does not delete the application's record before writing its replacement.
- On installation failure, it attempts to restore the previous NS record and remove newly introduced metadata/content. If rollback fails, it retains potentially referenced new contents rather than deleting them and leaving dangling references.
- NCM and NS commits are not a single atomic transaction. Power loss can interrupt the process; this is not a guarantee of power-loss-safe installation.
- Old releases' registered content is currently retained. Repeated updates can consume additional storage; safe content reclamation is a separate feature.
- Cancellation is supported before successful completion. Once the new application record is committed successfully, the client must treat installation as completed and restart.
- Native restart requires a compatible installed-application environment. Restart failure is logged; downloading through an NRO does not provide the same native self-install capability.
- HTTPS and SHA-256 checks detect transport/file corruption and internal inconsistencies. An unsigned manifest and hashes stored in the same package do not independently authenticate its publisher. A reusable production updater should add separately trusted signed release metadata if publisher authentication is required.

Local installation and restart diagnostics are appended to:

```text
sdmc:/switch/Connections/update.log
```

## Build and release commands

From this project's root in PowerShell:

```powershell
# Clean build of both deployable artifacts, including final NSP verification.
.\packaging\build.ps1 -Clean -Release

# Development NRO build only.
.\packaging\build.ps1
```

With devkitPro configured in a compatible shell, the Makefile also exposes `make nro`, `make nsp`, and `make release` targets. The PowerShell wrapper configures local tool/runtime paths and moves intermediate ELF/NSO/NACP outputs into `build/artifacts`.

Deployable outputs are `Connections.nro` and `Connections.nsp` at the project root. Temporary NSP staging is removed after successful verification; there is one final NSP.

For each release: bump versions, clean-build, verify the artifact, replace the hosted release asset, then manually confirm version discovery, install, restart, and the new displayed version on physical hardware.

## Final-package verification

`packaging/verify_nsp.py` extracts the actual final NSP/NCAs and checks their contents. A successful tool exit or the existence of an `.nsp` alone is insufficient.

Verify at least:

- Program, Control, and Meta contents are present with correct types and IDs.
- ExeFS contains the native program and valid NPDM with consistent title identity.
- Control contains a valid NACP, expected name/publisher/version, and startup account policy.
- The icon exists inside Control and is a valid 256×256 JPEG for the supported language.
- CNMT references the actual generated contents with correct sizes, hashes, digest, and application metadata.
- The manifest agrees with title identity and CNMT/native/display versions.
- The chosen title ID is distinct from unrelated applications installed on the target console.

Build-time verification does not prove that a HOME Menu launch, user-selection prompt, native installation, or restart succeeds on a specific Switch. Those require physical-device validation. This workflow has been compiled and its generated packages inspected; native self-update behavior should not be treated as hardware-tested solely from those checks.

## Implementation reference

| File | Responsibility |
| --- | --- |
| `Makefile` | Shared compilation and NRO/NSO/package targets |
| `packaging/build.ps1` | Toolchain setup, clean/release build, intermediate organization |
| `packaging/build_nsp.py` | NPDM/NACP, NCA/CNMT generation, manifest, final package |
| `packaging/prepare_icon.ps1` | HOME Menu JPEG conversion |
| `packaging/verify_nsp.py` | Final extracted-package validation |
| `source/core/version.h` | Display version and native package revision |
| `source/core/nsp_update.c` | Bounded NSP-index and remote-manifest parsing |
| `source/platform/platform_updates.c` | Launch checks, HTTPS worker, temporary download, progress |
| `source/platform/platform_nsp_install.c` | Own-title validation, NCM/NS installation and rollback |
| `source/main.c` | Resource cleanup followed by native application restart |
