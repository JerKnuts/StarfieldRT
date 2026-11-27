ScriptName SFRT_OptionsQuest extends Quest
{Starfield RT: copies the Gameplay Options choices into globals that the SFSE plugin reads every 30 frames.
0.5.92: Ray Tracing On / Off. 0.5.94: + Ray Tracing Quality (Low / Medium / High / Ultra).}

GameplayOption Property SFRT_GPO_RayTracing Auto Const Mandatory
{Ray Tracing: 0 off, 1 on}
GlobalVariable Property SFRT_RayTracing Auto Const Mandatory
GameplayOption Property SFRT_GPO_RTQuality Auto Const Mandatory
{Ray Tracing Quality: 1 low, 2 medium, 3 high, 4 ultra}
GlobalVariable Property SFRT_RTQuality Auto Const Mandatory

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
    SFRT_RayTracing.SetValue(SFRT_GPO_RayTracing.GetValue())
    SFRT_RTQuality.SetValue(SFRT_GPO_RTQuality.GetValue())
EndFunction
