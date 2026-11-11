# What this port needs from the player's game copy. Read by tools\installer\gamefiles.ps1
# (used by setup.ps1 and the installer). Paths are relative to the folder that contains PS3_GAME.
@{
    GameName    = 'Resistance 3'
    TitleId     = 'BCUS98176'
    RegionLabel = 'US'
    AppVer      = '01.00'
    TitleMatch  = 'Resistance.?3'                 # other regions of the same game (for a clearer message)
    Required    = @('PS3_GAME/PARAM.SFO', 'PS3_GAME/USRDIR/EBOOT.BIN')
    Program     = @('PS3_GAME/USRDIR/EBOOT.BIN')   # the PS3 program file Setup decrypts
    Sha1        = @{
        'PS3_GAME/USRDIR/EBOOT.BIN' = '43bad7101e8e54b1f37f65674affef59bfb2ff53'
    }
    MinBytes    = 33500000000
    SizeLabel   = '34.6 GB'
    BuildBytes  = 12000000000                    # RPCS3 source, generated code, the build and the shader cache
    SaveFolder  = 'Resistance 3'                 # %USERPROFILE%\Saved Games\<this>
}