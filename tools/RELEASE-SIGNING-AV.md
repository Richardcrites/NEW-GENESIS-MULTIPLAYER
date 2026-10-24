# PULSAR - Release signing & antivirus checklist

Goal: keep Windows Defender / SmartScreen from blocking the tool exes and the mod DLL.
There is no single switch. This is the ordered, mostly-free playbook.

## Why it gets flagged
- **SmartScreen "Unknown publisher"** - the `.exe` is unsigned and has no reputation. Every
  first-time user sees "Windows protected your PC". This is the #1 cause of support pings.
- **Defender heuristics** - two risk factors:
  1. the tool is a **self-contained single-file** publish (a self-extracting bundle), and
  2. the mod **DLL injects into a game** (classic SFSE-plugin pattern).
  Both are most likely to trip while the file is still rare (low prevalence).

## What actually helps (in order)

### 1. Code signing (the real lever) - `tools/sign.ps1`
Sign every shipped binary with an Authenticode cert AFTER publish, BEFORE zipping:

    cd tools
    .\sign.ps1 -Pfx C:\secret\pulsar.pfx -Password '***' `
       -Files ..\lobby\_dist\Pulsar-v0.2.0\Pulsar.exe, ..\lobby\_dist\Pulsar-v0.2.0\PulsarCompanion.exe, <PULSAR.dll>

- **OV cert** (~70-200 EUR/yr): removes the blank-publisher text; SmartScreen still warns
  until reputation accrues - so always re-sign with the SAME cert (reputation accumulates).
- **EV cert** (~300 EUR/yr + hardware token / cloud HSM): near-instant SmartScreen trust.
- The `.pfx`/password are SECRETS - never commit them, never ship them. You sign WITH the
  cert; you do not distribute it.

### 2. Pre-flight scan (free, every release)
Upload each binary to https://www.virustotal.com before publishing. Catch flags before
users do. A couple of no-name engines flagging a self-extractor is normal; Defender +
the big names must be clean.

### 3. Microsoft false-positive submission (free, the fix when Defender DOES eat it)
If Defender quarantines/deletes a binary, submit it here (as the software developer):
https://www.microsoft.com/en-us/wdsi/filesubmission
Turnaround is usually 1-3 days; the detection is removed cloud-side for everyone. Do this
proactively the moment a release binary trips Defender.

### 4. Clean version metadata (done in the csprojs)
Company / Product / Description / Copyright / FileVersion are set on `Pulsar.csproj` and
`Companion.csproj`. A binary with a blank FileDescription/Company looks anonymous and is
more heuristic-prone. (Necessary groundwork for signing; not a fix by itself.)

### 5. Do NOT pack/obfuscate the DLL
VMProtect/Themida and similar packers massively INCREASE AV false positives (self-injecting
DLL + packer = textbook malware shape). The existing Tier-0/1 hardening (no packer) is
correct - keep it.

### 6. Packaging escalation (if 1-3 are not enough)
The self-contained single-file is the most heuristic-prone shape. If signing + submission
still isn't enough, in rough order of effort:
- ship a **signed Inno Setup installer** wrapping the published folder (installers read as
  more trustworthy than a bare self-extracting exe), or
- publish self-contained **as a folder** (not single-file) so there is no runtime
  self-extraction, or
- publish **framework-dependent** (needs .NET 8 on the user's PC - adds an install step,
  so only if the heuristics demand it).

## User-facing SmartScreen note (put this on the pulsar-releases README / download page)
> Windows may show "Windows protected your PC" the first time you run PULSAR, because the
> app is new and not yet widely downloaded. Click **More info → Run anyway**. This warning
> goes away as more people run it. If your antivirus quarantines a file, restore it and add
> an exclusion for the PULSAR folder, or report it to us so we can get it cleared.
