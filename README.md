# UWPInjector
Small script to inject a DLL into a UWP application process.
 
This is largely based on [crosire](https://github.com/crosire)'s [/tools/injector.cpp](https://github.com/crosire/reshade/blob/main/tools/injector.cpp) script that was made for [ReShade](https://github.com/crosire/reshade). I added an INI file for specifying the DLL and process name, and optionally launching the UWP app from the script, for immediate injection.

## Usage
Put the DLL and process names in the INI, and run the script. The DLL should be in the same directory as the script. To make the script launch the app, put the app ID in the INI. An example ID is shown in the INI.

The app ID should look like `<PublisherName>.<AppName>_<PublisherID>!<AppEntryPoint>`

## Credits
[ReShade](https://github.com/crosire/reshade) for the original script.
