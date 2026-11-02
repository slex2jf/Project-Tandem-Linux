# Project Tandem — Linux

**Unofficial Linux Adaptation of Project Tandem**

A Linux-focused adaptation of Project Tandem featuring a graphical launcher, local compilation support, and a simplified way to build and launch Mario Kart: Double Dash!! on Linux.

This repository is an independent community adaptation. Full credit for the original project belongs to its developers and contributors.

## Latest Release — TANDEM V1.1

**[Download TANDEM V1.1](https://github.com/slex2jf/Project-Tandem-Linux/releases/tag/V.1.1)**

Previous release: [Linux V1.0](https://github.com/slex2jf/Project-Tandem-Linux/releases/tag/V.1.0)

## Features

- Native graphical launcher for Linux.
- Local game recompilation workflow.
- Smart Build support to avoid unnecessary recompilation.
- Linux setup and launch scripts.
- Support for selecting a compatible game image.
- No game ISO included.

## Installation

### 1. Download

Download the latest release and extract the ZIP archive.

### 2. Install Dependencies

On Ubuntu or Linux Mint:

    sudo apt install python3 python3-gi gir1.2-gtk-3.0 gir1.2-webkit2-4.1 cmake ninja-build build-essential zenity

Additional dependencies may be required depending on your Linux distribution.

### 3. Launch

Open a terminal inside the extracted project directory and run:

    bash ./Abrir-Tandem.sh

### 4. Build and Play

1. Select your legally obtained compatible Mario Kart: Double Dash!! ISO/GCM.
2. Click **RECOMPILE GAME** to build the game locally.
3. Once compilation finishes, click **EXECUTE GAME**.
4. On subsequent launches, Smart Build can skip an unnecessary rebuild.

## Credits and Special Thanks

### Original Project Tandem — BadCompany94

Special thanks to **badcompany94-rcmp** and all original contributors for creating and developing Project Tandem.

Their work made this Linux adaptation possible.

**Original Developer:** https://github.com/badcompany94-rcmp

### GameCube Recompilation — Ethan Nicholas

Special thanks to **Ethan Nicholas** for his work on GameCube recompilation technology, including `gcn-recomp` and `mariokartdd-recomp`.

His contributions to the GameCube recompilation community are greatly appreciated.

**Developer:** https://github.com/ethannicholas

### Linux Adaptation — slex2jf

Linux integration, graphical launcher customization, Smart Build improvements, and Linux release packaging by **slex2jf**.

**GitHub:** https://github.com/slex2jf

## Disclaimer

This is an unofficial community adaptation and is not the original Project Tandem repository.

Mario Kart: Double Dash!! and related intellectual property belong to Nintendo and their respective owners.

This project is not affiliated with or endorsed by Nintendo.

No game ISO or disc image is included. Users must provide their own legally obtained compatible game image.

Please respect the original project's licenses, copyright notices, and third-party acknowledgements.

## Support

For issues specifically related to this Linux adaptation, please use the Issues section of this repository.

For original Project Tandem development, refer to the original developers and their official repositories.
