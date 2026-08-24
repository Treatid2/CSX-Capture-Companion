using Mutagen.Bethesda;
using Mutagen.Bethesda.Plugins;
using Mutagen.Bethesda.Plugins.Records;
using Mutagen.Bethesda.Skyrim;

const string PluginName = "CSXCaptureCompanion.esp";
const ushort RecordFormVersion = 44;
var voiceEquipType = new FormKey(ModKey.FromNameAndExtension("Skyrim.esm"), 0x25BEE);

var recordIds = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase)
{
    ["CSXCaptureQuest"] = 0x800,
    ["CSXCaptureScreenshotEffect"] = 0x801,
    ["CSXCaptureToggleEffect"] = 0x802,
    ["CSXCaptureComposeEffect"] = 0x803,
    ["CSXCaptureScreenshotPower"] = 0x804,
    ["CSXCaptureTogglePower"] = 0x805,
    ["CSXCaptureComposePower"] = 0x806,
};

if (args.Length == 2 && args[0] == "--verify")
{
    return VerifyPlugin(Path.GetFullPath(args[1]), recordIds);
}

if (args.Length != 1)
{
    Console.Error.WriteLine("Usage: BuildPlugin <output CSXCaptureCompanion.esp>");
    Console.Error.WriteLine("       BuildPlugin --verify <CSXCaptureCompanion.esp>");
    return 2;
}

var outputPath = Path.GetFullPath(args[0]);
var modKey = ModKey.FromNameAndExtension(PluginName);
var mod = new SkyrimMod(modKey, SkyrimRelease.SkyrimSE)
{
    IsSmallMaster = true,
};
mod.ModHeader.Author = "Treatid2";
mod.ModHeader.Description = "CSX lossless capture API demonstrator";
mod.ModHeader.Stats.Version = 1.7f;

var quest = new Quest(new FormKey(modKey, recordIds["CSXCaptureQuest"]), SkyrimRelease.SkyrimSE)
{
    EditorID = "CSXCaptureQuest",
    Name = "CSX Capture Companion MCM",
    Flags = (Quest.Flag)17, // Proven SkyUI pattern: Start Game Enabled | Run Once.
    Priority = 0,
    QuestFormVersion = 255,
    FormVersion = RecordFormVersion,
    NextAliasID = 1,
};
mod.Quests.Add(quest);

var screenshotEffect = AddCaptureEffect(
    mod,
    recordIds["CSXCaptureScreenshotEffect"],
    "CSXCaptureScreenshotEffect",
    "Capture Screenshot",
    0);
var toggleEffect = AddCaptureEffect(
    mod,
    recordIds["CSXCaptureToggleEffect"],
    "CSXCaptureToggleEffect",
    "Start or Stop Frame Capture",
    1);
var composeEffect = AddCaptureEffect(
    mod,
    recordIds["CSXCaptureComposeEffect"],
    "CSXCaptureComposeEffect",
    "Compose Latest Capture",
    2);

var screenshotPower = AddPower(
    mod,
    recordIds["CSXCaptureScreenshotPower"],
    "CSXCaptureScreenshotPower",
    "Capture Screenshot",
    "Capture one lossless CSX screenshot.",
    screenshotEffect,
    voiceEquipType);
var togglePower = AddPower(
    mod,
    recordIds["CSXCaptureTogglePower"],
    "CSXCaptureTogglePower",
    "Start/Stop Frame Capture",
    "Start or stop a lossless CSX frame sequence.",
    toggleEffect,
    voiceEquipType);
var composePower = AddPower(
    mod,
    recordIds["CSXCaptureComposePower"],
    "CSXCaptureComposePower",
    "Compose Latest Capture",
    "Compose the latest completed frame sequence into video.",
    composeEffect,
    voiceEquipType);

var mcmScript = new ScriptEntry
{
    Name = "CSXCaptureMCM",
    Flags = ScriptEntry.Flag.Local,
};
AddObjectProperty(mcmScript, "ScreenshotPower", screenshotPower.FormKey);
AddObjectProperty(mcmScript, "ToggleCapturePower", togglePower.FormKey);
AddObjectProperty(mcmScript, "ComposeVideoPower", composePower.FormKey);

var questAdapter = new QuestAdapter
{
    Version = 5,
    ObjectFormat = 2,
};
questAdapter.Scripts.Add(mcmScript);
quest.VirtualMachineAdapter = questAdapter;

// SkyUI's player alias forwards load-game events to SKI_ConfigBase. This is the
// same small, proven MCM structure used by ThrowingStuffVR.
var playerAlias = new QuestAlias
{
    ID = 0,
    Type = QuestAlias.TypeEnum.Reference,
    Name = "PlayerAlias",
    Flags = 0,
};
playerAlias.ForcedReference.SetTo(FormKey.Factory("000014:Skyrim.esm"));
quest.Aliases.Add(playerAlias);

var aliasProperty = new ScriptObjectProperty
{
    Name = string.Empty,
    Alias = 0,
    Unused = 0,
};
aliasProperty.Object.SetTo(quest.FormKey);

var loadGameAliasAdapter = new QuestFragmentAlias
{
    Property = aliasProperty,
    Version = 5,
    ObjectFormat = 2,
};
loadGameAliasAdapter.Scripts.Add(new ScriptEntry
{
    Name = "SKI_PlayerLoadGameAlias",
    Flags = ScriptEntry.Flag.Local,
});
questAdapter.Aliases.Add(loadGameAliasAdapter);

Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
mod.WriteToBinary(outputPath);
Console.WriteLine($"Wrote {mod.EnumerateMajorRecords().Count()} records to {outputPath}");
return VerifyPlugin(outputPath, recordIds);

static MagicEffect AddCaptureEffect(
    SkyrimMod mod,
    uint id,
    string editorId,
    string name,
    int action)
{
    var effect = new MagicEffect(new FormKey(mod.ModKey, id), SkyrimRelease.SkyrimSE)
    {
        EditorID = editorId,
        Name = name,
        FormVersion = RecordFormVersion,
        BaseCost = 0.0f,
        MagicSkill = ActorValue.None,
        ResistValue = ActorValue.None,
        Flags = MagicEffect.Flag.NoDuration |
                MagicEffect.Flag.NoMagnitude |
                MagicEffect.Flag.NoArea |
                MagicEffect.Flag.Painless |
                MagicEffect.Flag.NoHitEffect |
                MagicEffect.Flag.NoDeathDispel,
        Archetype = new MagicEffectArchetype(MagicEffectArchetype.TypeEnum.Script),
        CastType = CastType.FireAndForget,
        TargetType = TargetType.Self,
    };

    var script = new ScriptEntry
    {
        Name = "CSXCapturePowerEffect",
        Flags = ScriptEntry.Flag.Local,
    };
    script.Properties.Add(new ScriptIntProperty
    {
        Name = "Action",
        Data = action,
    });
    var adapter = new VirtualMachineAdapter
    {
        Version = 5,
        ObjectFormat = 2,
    };
    adapter.Scripts.Add(script);
    effect.VirtualMachineAdapter = adapter;
    mod.MagicEffects.Add(effect);
    return effect;
}

static Spell AddPower(
    SkyrimMod mod,
    uint id,
    string editorId,
    string name,
    string description,
    MagicEffect baseEffect,
    FormKey voiceEquipType)
{
    var spell = new Spell(new FormKey(mod.ModKey, id), SkyrimRelease.SkyrimSE)
    {
        EditorID = editorId,
        Name = name,
        Description = description,
        FormVersion = RecordFormVersion,
        Type = SpellType.LesserPower,
        Flags = SpellDataFlag.ManualCostCalc,
        BaseCost = 0,
        ChargeTime = 0.0f,
        CastType = CastType.FireAndForget,
        TargetType = TargetType.Self,
        CastDuration = 0.0f,
        Range = 0.0f,
    };
    spell.EquipmentType.SetTo(voiceEquipType);
    var effect = new Effect
    {
        Data = new EffectData
        {
            Magnitude = 0.0f,
            Area = 0,
            Duration = 0,
        },
    };
    effect.BaseEffect.SetTo(baseEffect.FormKey);
    spell.Effects.Add(effect);
    mod.Spells.Add(spell);
    return spell;
}

static void AddObjectProperty(ScriptEntry script, string name, FormKey target)
{
    var property = new ScriptObjectProperty
    {
        Name = name,
        Alias = -1,
        Unused = 0,
    };
    property.Object.SetTo(target);
    script.Properties.Add(property);
}

static int VerifyPlugin(string pluginPath, IReadOnlyDictionary<string, uint> expectedIds)
{
    var expectedVoiceEquipType = new FormKey(ModKey.FromNameAndExtension("Skyrim.esm"), 0x25BEE);
    if (!File.Exists(pluginPath))
    {
        Console.Error.WriteLine($"Plugin does not exist: {pluginPath}");
        return 3;
    }

    var mod = SkyrimMod.CreateFromBinary(pluginPath, SkyrimRelease.SkyrimSE);
    var errors = new List<string>();
    var records = mod.EnumerateMajorRecords().ToArray();
    if (!mod.IsSmallMaster)
        errors.Add("Plugin is not ESL-flagged.");
    if (records.Length != expectedIds.Count)
        errors.Add($"Expected {expectedIds.Count} records, found {records.Length}.");

    foreach (var expected in expectedIds)
    {
        var record = records.SingleOrDefault(record =>
            string.Equals(record.EditorID, expected.Key, StringComparison.OrdinalIgnoreCase));
        if (record is null)
        {
            errors.Add($"Missing record {expected.Key}.");
        }
        else if (record.FormKey.ID != expected.Value)
        {
            errors.Add($"{expected.Key} has FormID {record.FormKey.ID:X}, expected {expected.Value:X}.");
        }
    }

    var quest = mod.Quests.SingleOrDefault(record => record.EditorID == "CSXCaptureQuest");
    if (quest?.VirtualMachineAdapter is null || quest.Aliases.Count != 1)
        errors.Add("MCM quest does not contain the required script and player alias structure.");
    else
    {
        var mcm = quest.VirtualMachineAdapter.Scripts.SingleOrDefault(script => script.Name == "CSXCaptureMCM");
        var properties = mcm?.Properties.Select(property => property.Name).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var name in new[] { "ScreenshotPower", "ToggleCapturePower", "ComposeVideoPower" })
        {
            if (properties?.Contains(name) != true)
                errors.Add($"MCM quest is missing property {name}.");
        }
        if (!quest.VirtualMachineAdapter.Aliases.SelectMany(alias => alias.Scripts)
                .Any(script => script.Name == "SKI_PlayerLoadGameAlias"))
            errors.Add("MCM quest is missing SKI_PlayerLoadGameAlias.");
    }

    foreach (var (editorId, action) in new[]
    {
        ("CSXCaptureScreenshotEffect", 0),
        ("CSXCaptureToggleEffect", 1),
        ("CSXCaptureComposeEffect", 2),
    })
    {
        var effect = mod.MagicEffects.SingleOrDefault(record => record.EditorID == editorId);
        var actionProperty = effect?.VirtualMachineAdapter?.Scripts
            .SingleOrDefault(script => script.Name == "CSXCapturePowerEffect")?
            .Properties.OfType<ScriptIntProperty>()
            .SingleOrDefault(property => property.Name == "Action");
        if (actionProperty?.Data != action)
            errors.Add($"{editorId} does not bind Action={action}.");
    }

    foreach (var editorId in new[]
    {
        "CSXCaptureScreenshotPower",
        "CSXCaptureTogglePower",
        "CSXCaptureComposePower",
    })
    {
        var spell = mod.Spells.SingleOrDefault(record => record.EditorID == editorId);
        if (spell?.Type != SpellType.LesserPower)
            errors.Add($"{editorId} is not a Lesser Power.");
        if (spell?.EquipmentType.FormKey != expectedVoiceEquipType)
            errors.Add($"{editorId} does not use Skyrim's Voice equip type.");
    }

    if (errors.Count != 0)
    {
        foreach (var error in errors)
            Console.Error.WriteLine(error);
        return 4;
    }

    Console.WriteLine("Verified ESL flag, 7 stable records, MCM alias, Voice-equipped Lesser Powers, and action bindings.");
    return 0;
}
