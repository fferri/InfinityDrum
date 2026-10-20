#!/bin/sh
#
# Install InfinityDrum on macOS.
#
#   ./install-macos.sh                  install the VST3 and the Audio Unit where DAWs look
#   ./install-macos.sh /path/to/VST3    install only the VST3, into that folder
#
# This does two things a drag-and-drop does not. It puts the plugins where DAWs look for
# them, and it clears the quarantine flag that a browser download leaves behind - without
# that, Gatekeeper refuses to load them and your DAW simply will not see them.
#
# Nothing here needs sudo, and nothing outside the destination folders is touched.

set -eu

here=$(cd "$(dirname "$0")" && pwd)

vst_source="$here/InfinityDrum.vst3"
au_source="$here/InfinityDrum.component"

vst_destination="$HOME/Library/Audio/Plug-Ins/VST3"
au_destination="$HOME/Library/Audio/Plug-Ins/Components"

# Copies one bundle into place. Refuses any target that is not one of the two bundle types,
# so a mistyped path can never hand rm -rf a directory holding something else.
install_bundle()
{
    source_bundle="$1"
    target_directory="$2"
    installed="$target_directory/$(basename "$source_bundle")"

    case "$installed" in
        *.vst3 | *.component) ;;
        *) echo "error: refusing to install to $installed" >&2; exit 1 ;;
    esac

    if [ -z "$target_directory" ] || [ "$target_directory" = "/" ]; then
        echo "error: refusing to install into '$target_directory'" >&2
        exit 1
    fi

    mkdir -p "$target_directory"

    # Replaced rather than merged, so a previous version cannot leave stale files behind.
    if [ -e "$installed" ]; then
        echo "  replacing $installed"
        rm -rf "$installed"
    fi

    cp -R "$source_bundle" "$installed"

    # A browser download sets this attribute and Gatekeeper will refuse to load quarantined
    # code. A terminal download sets nothing, so this is a no-op in that case.
    xattr -dr com.apple.quarantine "$installed" 2>/dev/null || true

    echo "  installed $installed"
}

if [ "$#" -ge 1 ] && [ -n "$1" ]; then
    # An explicit destination only makes sense for one bundle, so this installs the VST3 and
    # leaves the Audio Unit alone.
    vst_destination="$1"
    au_destination=""
fi

if [ ! -d "$vst_source" ] && [ ! -d "$au_source" ]; then
    echo "error: no InfinityDrum.vst3 or InfinityDrum.component next to this script." >&2
    echo "       Unzip the download first, then run the script from inside it." >&2
    exit 1
fi

echo "InfinityDrum installer"

if [ -d "$vst_source" ]; then
    install_bundle "$vst_source" "$vst_destination"
else
    echo "  (no VST3 in this package)"
fi

if [ -n "$au_destination" ]; then
    if [ -d "$au_source" ]; then
        install_bundle "$au_source" "$au_destination"
    else
        echo "  (no Audio Unit in this package)"
    fi
fi

echo
echo "Done. Rescan plugins in your DAW and InfinityDrum will appear as an instrument."
echo "If it does not, check that the DAW is looking in the folders above."
