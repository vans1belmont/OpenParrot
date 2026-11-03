project "OpenParrotInput"
	targetname "OpenParrotInput"
	language "C++"
	kind "ConsoleApp"
	removeplatforms { "x64" }

	characterset "MBCS"

	files
	{
		"src/**.cpp", "src/**.h"
	}

	includedirs { "src" }

	links { "winmm" }

	prebuildcommands {
		"if not exist $(TargetDir)output mkdir $(TargetDir)output",
	}

	postbuildcommands {
		"if exist $(TargetDir)OpenParrotInput.exe xcopy /y $(TargetDir)OpenParrotInput.exe $(TargetDir)output\\"
	}
