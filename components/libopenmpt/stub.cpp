// libopenmpt stub - add libopenmpt source to enable
// See README.md for setup instructions

// This file prevents build errors when libopenmpt source is not yet added.
// Once libopenmpt source is added (via submodule or manual copy), this file
// will be ignored and the actual libopenmpt sources will be compiled instead.

// To add libopenmpt:
// 1. cd components/libopenmpt
// 2. git submodule add https://github.com/OpenMPT/openmpt.git libopenmpt
// 3. cd libopenmpt && git checkout libopenmpt-0.7.0
// 4. Return to project root and run: make build
