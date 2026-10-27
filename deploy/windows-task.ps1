# Registers the agent as a Scheduled Task that starts at log on and restarts on failure.
# Run in an elevated PowerShell. Adjust $Agent and the environment values first.
$Agent = "C:\meridian\meridian-agent.exe"
$Conf  = "C:\meridian\agent.conf"
$Action  = New-ScheduledTaskAction -Execute $Agent -Argument "-c `"$Conf`""
$Trigger = New-ScheduledTaskTrigger -AtLogOn
$Settings = New-ScheduledTaskSettingsSet -RestartCount 999 -RestartInterval (New-TimeSpan -Minutes 1) `
  -ExecutionTimeLimit ([TimeSpan]::Zero) -StartWhenAvailable
Register-ScheduledTask -TaskName "Meridian Core Agent" -Action $Action -Trigger $Trigger -Settings $Settings
