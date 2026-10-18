#include "Settings.h"

namespace df
{

namespace
{
    const juce::Identifier rootType { "INFINITYDRUM_SETTINGS" };
    const char* noteMapProperty = "noteMap";
    const char* fileName = "settings.xml";
}

namespace
{
    juce::File& overrideDirectory()
    {
        static juce::File directory;
        return directory;
    }
}

void setSettingsDirectoryOverride (juce::File directory)
{
    overrideDirectory() = std::move (directory);
}

namespace
{
    /** Carries a pre-rename installation over the first time the new name is used.

        The product used to be called DrumForge, and its kits and settings lived under that
        name. Without this, renaming the plugin would silently orphan every kit the user had
        saved. Copy rather than move: if anything is wrong with the migration, the original
        is still there. Runs once per process, and never when a directory is overridden,
        which is how the tests and the offline tools stay isolated.
    */
    void migrateLegacyDirectoryOnce()
    {
        static const bool done = []
        {
            const auto base = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory);
            const auto current = base.getChildFile ("InfinityDrum");
            const auto legacy = base.getChildFile ("DrumForge");

            if (! current.exists() && legacy.isDirectory())
                legacy.copyDirectoryTo (current);

            return true;
        }();

        juce::ignoreUnused (done);
    }
}

juce::File defaultSettingsDirectory()
{
    if (overrideDirectory() != juce::File())
        return overrideDirectory();

    migrateLegacyDirectoryOnce();

    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("InfinityDrum");
}

NoteMap loadNoteMapSetting (juce::File directory)
{
    const auto file = directory.getChildFile (fileName);

    if (! file.existsAsFile())
        return NoteMap::generalMidi;

    auto xml = juce::XmlDocument::parse (file);

    // A settings file written before the rename keeps working; it only holds the note map.
    const bool known = xml != nullptr
                         && (xml->hasTagName (rootType) || xml->hasTagName ("DRUMFORGE_SETTINGS"));

    if (! known)
        return NoteMap::generalMidi;

    const auto stored = xml->getStringAttribute (noteMapProperty);

    for (int i = 0; i < kNumNoteMaps; ++i)
    {
        const auto map = (NoteMap) i;

        if (stored == noteMapName (map))
            return map;
    }

    return NoteMap::generalMidi;
}

bool saveNoteMapSetting (NoteMap map, juce::File directory)
{
    if (const auto created = directory.createDirectory(); created.failed())
        return false;

    juce::XmlElement xml (rootType);
    xml.setAttribute (noteMapProperty, noteMapName (map));

    return xml.writeTo (directory.getChildFile (fileName));
}

} // namespace df
