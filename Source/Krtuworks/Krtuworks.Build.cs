using System.IO;
using System.Text.RegularExpressions;
using UnrealBuildTool;

// The Krtuworks SDK for Unreal (5.3+): the engine-independent core (Public/KrtuworksCore, Private/KrtuworksCore:
// standard C++ only, tested outside the engine with g++) and the thin engine layer around it (HTTP, file and
// secret storage per platform, the phone sign-in, Blueprint nodes, the ready-made screens in Slate).
public class Krtuworks : ModuleRules
{
	public Krtuworks(ReadOnlyTargetRules Target) : base(Target)
	{
		// The core is plain C++: no engine header is forced into its files, and its files are not merged
		// (their file-local helpers keep their own names).
		PCHUsage = PCHUsageMode.NoPCHs;
		bUseUnity = false;
		// The core reports failures as krtu::Error exceptions; the engine layer catches them at the edge.
		bEnableExceptions = true;

		PublicDependencyModuleNames.AddRange(new string[] { "Core", "CoreUObject", "Engine", "DeveloperSettings", "SlateCore", "UMG" });
		PrivateDependencyModuleNames.AddRange(new string[] { "HTTP", "Projects", "ApplicationCore", "EngineSettings", "ImageWrapper", "InputCore", "Slate" });
		PrivateIncludePaths.Add(Path.Combine(ModuleDirectory, "Private", "KrtuworksCore"));

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// DPAPI seals the secrets' key; Winsock is the desktop sign-in's loopback listener.
			PublicSystemLibraries.AddRange(new string[] { "crypt32.lib", "ws2_32.lib" });
		}
		else if (Target.Platform == UnrealTargetPlatform.Mac)
		{
			PublicFrameworks.AddRange(new string[] { "Security", "CoreFoundation" });
		}
		else if (Target.Platform == UnrealTargetPlatform.IOS)
		{
			PublicFrameworks.AddRange(new string[] { "Security", "CoreFoundation", "AuthenticationServices", "UIKit", "Foundation" });
			// The sign-in session's completion block holds the session weakly, which needs ARC.
			bEnableObjCAutomaticReferenceCounting = true;
		}
		else if (Target.Platform == UnrealTargetPlatform.Android)
		{
			PrivateDependencyModuleNames.Add("Launch");
			AdditionalPropertiesForReceipt.Add("AndroidPlugin", Path.Combine(ModuleDirectory, "Krtuworks_UPL.xml"));
		}

		// The app's krtuworks-config.json (from the partner console's Credentials page) ships with the game,
		// next to the .uproject; a packaged build without it could never sign anyone in, so it stops here.
		if (Target.ProjectFile != null)
		{
			string config = Path.Combine(Target.ProjectFile.Directory.FullName, "krtuworks-config.json");
			if (File.Exists(config))
			{
				RuntimeDependencies.Add(config, StagedFileType.UFS);
				if (Target.Platform == UnrealTargetPlatform.Android)
				{
					RequireAndroidSlug(Target.ProjectFile.Directory.FullName, config);
				}
			}
			else if (Target.Type != TargetType.Editor)
			{
				throw new BuildException("Krtuworks SDK: put the app's krtuworks-config.json (from the Credentials page) next to the .uproject file.");
			}
		}
	}

	// The phone sign-in comes back on com.krtuworks.<slug>, added to the manifest from the AppSlug setting of
	// DefaultGame.ini. The editor copies the slug there when it opens the project; a build from a clean checkout
	// (CI, the command line) has not, and would ship an app the sign-in can never return to, so it stops here.
	private static void RequireAndroidSlug(string projectDirectory, string configFile)
	{
		Match inConfig = Regex.Match(File.ReadAllText(configFile), "\"slug\"\\s*:\\s*\"([^\"]+)\"");
		if (!inConfig.Success)
		{
			return;
		}
		string slug = inConfig.Groups[1].Value;
		string ini = Path.Combine(projectDirectory, "Config", "DefaultGame.ini");
		string text = File.Exists(ini) ? File.ReadAllText(ini) : "";
		Match inSettings = Regex.Match(text, "\\[/Script/Krtuworks\\.KrtuworksSettings\\][^\\[]*?^AppSlug=(\\S*)", RegexOptions.Multiline);
		if (!inSettings.Success || inSettings.Groups[1].Value != slug)
		{
			throw new BuildException("Krtuworks SDK: the Android sign-in return needs AppSlug=" + slug + " under [/Script/Krtuworks.KrtuworksSettings] in Config/DefaultGame.ini (open the project in the editor once, or add it by hand).");
		}
	}
}
