#include "KitLibrary.h"

namespace df
{

//==============================================================================
KitData captureKit (DrumKitProcessor& processor, const juce::String& name)
{
    KitData kit;
    kit.name = name;

    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& track = kit.tracks[(size_t) t];

        const auto read = [&processor, t] (const char* parameterName)
        {
            if (auto* value = processor.apvts.getRawParameterValue (pid::track (t, parameterName)))
                return value->load();

            return 0.0f;
        };

        track.model     = (int) std::lround (read (pid::model));
        track.distMode  = (int) std::lround (read (pid::dist));
        track.level     = read (pid::level);
        track.pan       = read (pid::pan);
        track.tune      = read (pid::tune);
        track.decay     = read (pid::decay);
        track.tone      = read (pid::tone);
        track.character = read (pid::character);
        track.sweep     = read (pid::sweep);
        track.drive     = read (pid::drive);
        track.send      = read (pid::send);

        for (int i = 0; i < kNumModTargets; ++i)
            track.depths[(size_t) i] = processor.getModulationDepth (t, i);
    }

    return kit;
}

void applyKit (DrumKitProcessor& processor, const KitData& kit)
{
    for (int t = 0; t < kNumTracks; ++t)
    {
        const auto& track = kit.tracks[(size_t) t];
        const auto id = [t] (const char* parameterName) { return pid::track (t, parameterName); };

        setParameterValue (processor.apvts, id (pid::model),     (float) track.model);
        setParameterValue (processor.apvts, id (pid::dist),      (float) track.distMode);
        setParameterValue (processor.apvts, id (pid::level),     track.level);
        setParameterValue (processor.apvts, id (pid::pan),       track.pan);
        setParameterValue (processor.apvts, id (pid::tune),      track.tune);
        setParameterValue (processor.apvts, id (pid::decay),     track.decay);
        setParameterValue (processor.apvts, id (pid::tone),      track.tone);
        setParameterValue (processor.apvts, id (pid::character), track.character);
        setParameterValue (processor.apvts, id (pid::sweep),     track.sweep);
        setParameterValue (processor.apvts, id (pid::drive),     track.drive);
        setParameterValue (processor.apvts, id (pid::send),      track.send);

        for (int i = 0; i < kNumModTargets; ++i)
            processor.setModulationDepth (t, i, track.depths[(size_t) i]);
    }
}

//==============================================================================
int numFactoryKits() noexcept
{
    return numKitPresets();
}

KitData factoryKit (int index)
{
    KitData kit;

    if (! juce::isPositiveAndBelow (index, numKitPresets()))
        return kit;

    const auto& preset = kitPreset (index);
    kit.name = preset.name;

    for (int t = 0; t < kNumTracks; ++t)
    {
        const auto& source = preset.tracks[(size_t) t];
        auto& track = kit.tracks[(size_t) t];

        track.model     = source.model;
        track.distMode  = source.distMode;
        track.level     = source.level;
        track.pan       = source.pan;
        track.tune      = source.tune;
        track.decay     = source.decay;
        track.tone      = source.tone;
        track.character = source.character;
        track.sweep     = source.sweep;
        track.drive     = source.drive;
        track.send      = source.send;

        for (int i = 0; i < kNumModTargets; ++i)
            track.depths[(size_t) i] = source.depths[i];
    }

    return kit;
}

//==============================================================================
std::unique_ptr<juce::XmlElement> kitToXml (const KitData& kit)
{
    auto xml = std::make_unique<juce::XmlElement> ("INFINITYDRUM_KIT");
    xml->setAttribute ("name", kit.name);
    xml->setAttribute ("version", 1);

    for (int t = 0; t < kNumTracks; ++t)
    {
        const auto& track = kit.tracks[(size_t) t];
        auto* element = xml->createNewChildElement ("TRACK");

        element->setAttribute ("index", t);
        element->setAttribute ("model", track.model);
        element->setAttribute ("dist", track.distMode);
        element->setAttribute ("level", (double) track.level);
        element->setAttribute ("pan", (double) track.pan);
        element->setAttribute ("tune", (double) track.tune);
        element->setAttribute ("decay", (double) track.decay);
        element->setAttribute ("tone", (double) track.tone);
        element->setAttribute ("character", (double) track.character);
        element->setAttribute ("sweep", (double) track.sweep);
        element->setAttribute ("drive", (double) track.drive);
        element->setAttribute ("send", (double) track.send);

        // Only non-zero depths are written, which keeps hand-edited files readable.
        for (int i = 0; i < kNumModTargets; ++i)
        {
            if (std::abs (track.depths[(size_t) i]) < 1.0e-6f)
                continue;

            auto* mod = element->createNewChildElement ("MOD");
            mod->setAttribute ("target", modTargetName (i));
            mod->setAttribute ("depth", (double) track.depths[(size_t) i]);
        }
    }

    return xml;
}

bool kitFromXml (const juce::XmlElement& xml, KitData& destination)
{
    // Kits saved before the rename carry the old tag; they are otherwise identical.
    if (! xml.hasTagName ("INFINITYDRUM_KIT") && ! xml.hasTagName ("DRUMFORGE_KIT"))
        return false;

    KitData kit;
    kit.name = xml.getStringAttribute ("name", "Untitled");

    for (auto* element : xml.getChildIterator())
    {
        if (! element->hasTagName ("TRACK"))
            continue;

        const int index = element->getIntAttribute ("index", -1);

        if (! juce::isPositiveAndBelow (index, kNumTracks))
            continue;

        auto& track = kit.tracks[(size_t) index];

        track.model     = element->getIntAttribute ("model", 0);
        track.distMode  = element->getIntAttribute ("dist", 0);
        track.level     = (float) element->getDoubleAttribute ("level", 0.0);
        track.pan       = (float) element->getDoubleAttribute ("pan", 0.0);
        track.tune      = (float) element->getDoubleAttribute ("tune", 0.0);
        track.decay     = (float) element->getDoubleAttribute ("decay", 0.5);
        track.tone      = (float) element->getDoubleAttribute ("tone", 0.5);
        track.character = (float) element->getDoubleAttribute ("character", 0.5);
        track.sweep     = (float) element->getDoubleAttribute ("sweep", 0.5);
        track.drive     = (float) element->getDoubleAttribute ("drive", 0.0);
        track.send      = (float) element->getDoubleAttribute ("send", 0.0);

        for (auto* mod : element->getChildIterator())
        {
            if (! mod->hasTagName ("MOD"))
                continue;

            const int target = modTargetFromName (mod->getStringAttribute ("target"));

            if (target >= 0)
                track.depths[(size_t) target] = (float) mod->getDoubleAttribute ("depth", 0.0);
        }
    }

    destination = std::move (kit);
    return true;
}

//==============================================================================
juce::File KitLibrary::defaultUserDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("InfinityDrum")
               .getChildFile ("Kits");
}

juce::String KitLibrary::sanitiseFileName (const juce::String& name)
{
    auto cleaned = name.retainCharacters (
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 -_()[]");

    cleaned = cleaned.trim();

    // Trailing dots and spaces are legal on macOS but not on Windows.
    while (cleaned.endsWithChar ('.') || cleaned.endsWithChar (' '))
        cleaned = cleaned.dropLastCharacters (1);

    return cleaned.isEmpty() ? juce::String ("Kit") : cleaned;
}

KitLibrary::KitLibrary (juce::File userDirectory)
    : userDir (std::move (userDirectory))
{
    rescan();
}

void KitLibrary::rescan()
{
    userKits.clear();

    if (! userDir.isDirectory())
        return;

    for (const auto& file : userDir.findChildFiles (juce::File::findFiles, false,
                                                    "*" + fileExtension()))
    {
        auto xml = juce::XmlDocument::parse (file);

        if (xml == nullptr)
            continue;

        KitData kit;

        if (! kitFromXml (*xml, kit))
            continue;

        if (kit.name.isEmpty())
            kit.name = file.getFileNameWithoutExtension();

        userKits.push_back ({ std::move (kit), file });
    }

    std::sort (userKits.begin(), userKits.end(),
               [] (const UserKit& a, const UserKit& b) { return a.data.name < b.data.name; });
}

int KitLibrary::getNumKits() const noexcept
{
    return numFactoryKits() + (int) userKits.size();
}

bool KitLibrary::isFactoryKit (int index) const noexcept
{
    return juce::isPositiveAndBelow (index, numFactoryKits());
}

juce::String KitLibrary::getName (int index) const
{
    if (isFactoryKit (index))
        return factoryKit (index).name;

    const int userIndex = index - numFactoryKits();

    if (! juce::isPositiveAndBelow (userIndex, (int) userKits.size()))
        return {};

    return userKits[(size_t) userIndex].data.name;
}

KitData KitLibrary::getKit (int index) const
{
    if (isFactoryKit (index))
        return factoryKit (index);

    const int userIndex = index - numFactoryKits();

    if (! juce::isPositiveAndBelow (userIndex, (int) userKits.size()))
        return {};

    return userKits[(size_t) userIndex].data;
}

int KitLibrary::indexOfName (const juce::String& name) const
{
    for (int i = 0; i < getNumKits(); ++i)
        if (getName (i) == name)
            return i;

    return -1;
}

juce::Result KitLibrary::saveKit (const KitData& kit)
{
    const auto name = kit.name.trim();

    if (name.isEmpty())
        return juce::Result::fail ("The kit needs a name");

    if (const auto created = userDir.createDirectory(); created.failed())
        return created;

    auto file = userDir.getChildFile (sanitiseFileName (name) + fileExtension());
    auto xml = kitToXml (kit);

    if (xml == nullptr || ! xml->writeTo (file))
        return juce::Result::fail ("Could not write " + file.getFullPathName());

    rescan();
    return juce::Result::ok();
}

juce::Result KitLibrary::deleteKit (int index)
{
    if (isFactoryKit (index))
        return juce::Result::fail ("Factory kits cannot be deleted");

    const int userIndex = index - numFactoryKits();

    if (! juce::isPositiveAndBelow (userIndex, (int) userKits.size()))
        return juce::Result::fail ("No such kit");

    const auto file = userKits[(size_t) userIndex].file;

    // To the trash, not gone: a mis-click on a kit you spent an hour on has to be
    // recoverable, and deleting a file is not an undoable action.
    if (! file.moveToTrash())
        if (! file.deleteFile())
            return juce::Result::fail ("Could not delete " + file.getFullPathName());

    rescan();
    return juce::Result::ok();
}

} // namespace df
