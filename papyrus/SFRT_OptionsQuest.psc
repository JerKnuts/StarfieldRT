ScriptName SFRT_OptionsQuest extends Quest
{Starfield RT (0.2.36): copies the Gameplay Options choices into globals that the SFSE plugin reads every frame.}

GameplayOption Property SFRT_GPO_Reflections Auto Const Mandatory
{RT Reflections: 0 off, 1 low, 2 medium, 3 high, 4 ultra}
GameplayOption Property SFRT_GPO_Shadows Auto Const Mandatory
{RT Shadows: 0 off, 1 low, 2 medium, 3 high, 4 ultra}
GlobalVariable Property SFRT_ReflectionQuality Auto Const Mandatory
GlobalVariable Property SFRT_ShadowQuality Auto Const Mandatory

Event OnQuestInit()
    Setup()
EndEvent

Event Actor.OnPlayerLoadGame(Actor akSender)
    Setup()
EndEvent

Event OnGameplayOptionChanged(GameplayOption[] aChangedOptions)
    Sync()
    GameplayOption.NotifyGameplayOptionUpdateFinished()
EndEvent

Function Setup()
    RegisterForGameplayOptionChangedEvent()
    RegisterForRemoteEvent(Game.GetPlayer(), "OnPlayerLoadGame")
    Sync()
EndFunction

Function Sync()
    SFRT_ReflectionQuality.SetValue(SFRT_GPO_Reflections.GetValue())
    SFRT_ShadowQuality.SetValue(SFRT_GPO_Shadows.GetValue())
EndFunction
