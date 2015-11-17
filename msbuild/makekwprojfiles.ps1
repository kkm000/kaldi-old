#
function Tokenize ($line) {
  # The powershell -split operator returns array of one empty string when 
  # splitting an empty string. D-oh!
  if ($line = $line.Trim()) {
    $line -split "\s+"
  } else {
    @()
  }
}

function Unwrap-And-Decomment-Lines {
  process {
    if ($_.EndsWith('\')) {
      $acc = $acc + $_.TrimEnd('\') + " "
    } else {
      echo (($acc + $_) -replace "\s*\#.*$","")
      $acc = ""
    }
  }
}

function ProcessVardef ($line) {
  ( (Tokenize $line.Substring($line.IndexOf('=') + 1).Trim()) + $accum ) | select -Unique
}

function ProcessDependency ($line, $filter = @(), $accum = @{}) {
  $xcolon = $line.IndexOf(':')
  $left  = Tokenize $line.Substring(0, $xcolon)  | select -Unique | where { $filter -contains $_ }
  $right = Tokenize $line.Substring($xcolon + 1) | select -Unique | where { $filter -contains $_ }
  $left |% { $accum[$_] = ($accum[$_] + $right) | select -Unique }
  return $accum
}

function ProcessMainMakefile {
  begin {
    $deps = @{}
  }
  process {
    switch -regex ($_.Trim()) {
      # Parse the SUBDIR string and handle only these subdirs and targets.
      "^\s*SUBDIRS\s*\+?\=" {
        $subdirs = ProcessVardef $_ $subdirs
        break;
      }
      # pick a build rule line and accumulate dependencies, filtered by what we only build.
      "^\s*(?>\w+\s*)+\:\s*(?>\w+\s*)*$" {
        $deps = ProcessDependency $_ $subdirs $deps
        break;
      }
    }
  }
  end {
    # make sure we have included all subdirs into deps keys
    $subdirs |% { $deps[$_] = $deps[$_] + @() }
    return $deps
  }
}

function BuildMainProject($data) {
  $file = @( @"
<?xml version="1.0"?>
<Project ToolsVersion="12.0" DefaultTargets="Test" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Target Name="Build">
    <PropertyGroup>
      <KaldiBuildDepth>1</KaldiBuildDepth>
    </PropertyGroup>
  </Target>

  <Target Name="Test">
    <PropertyGroup>
      <KaldiBuildDepth>3</KaldiBuildDepth>
    </PropertyGroup>
  </Target>

  <Target Name="build:_all_router_" DependsOnTargets="build:_all_"  AfterTargets="Build;Test" />
"@,

  (@"
  <Target Name="build:_all_" DependsOnTargets="{0}" />
"@ -f (($data.Keys |% { "build:$_" }) -join ";")),

  ($data.Keys |% { @"
  <Target Name="build:{0}" DependsOnTargets="{1}">
    <MSBuild Targets="Build" Projects="{0}/kaldi-{0}.kwproj" Properties="KaldiBuildDepth=`$(KaldiBuildDepth)"/>
  </Target>
"@ -f  $_, (($data[$_] |% { "build:$_" }) -join ";") }),

  (@"
  <Target Name="Clean">
    <MSBuild Targets="Clean" Projects="{0}" BuildInParallel="true" />
  </Target>
"@ -f (($data.Keys |% { "$_/kaldi-$_.kwproj" }) -join ";")),

  "</Project>" ) |% { $_ }

  $file -join "`n`n"
}

function BuildSubMakefile($proj) {
  begin {
    $binsrc = @()
    $libsrc = @()
    $testsrc = @()
    $libdeps = @()
  }
  process {
    switch -regex ($_.Trim()) {
      "^\s*OBJFILES\s*\+?\=" {
        if ($cuda) {
          $libsrc += ProcessVardef $_ |% { $_ -replace "\.o$",".cu" }
        } else {
          $libsrc += ProcessVardef $_ |% { $_ -replace "\.o$",".cc" }
        }
        break;
      }
      "^\s*TESTFILES\s*\+?\=" {
        $testsrc += ProcessVardef $_ |% { "$_.cc" }
        break;
      }
      "^\s*BINFILES\s*\+?\=" {
        $binsrc += ProcessVardef $_ |% { "$_.cc" }
        break;
      }
      "^\s*ADDLIBS\s*\+?\=" {
        $libdeps += ProcessVardef $_ |% { $_ -replace "\.a$","" }
        break;
      }
      "^\s*LIBNAME\s*\+?\=" {
        $libname = ProcessVardef $_
        if ($libname -ne "kaldi-$proj") {
          write-error "In ../src/%proj/Makefile LIBNAME defines the name '$libname', but this script only supports regular name 'kaldi-$proj' matching the directory name."
        }
        break;
      }
      "^ifeq.+\bCUDA\b.+\btrue\b" {
        $cuda = $true
        break;
      }
      "^endif\b" {
        $cuda = $false
        break;
      }
    }
  }
  end {
    echo '<?xml version="1.0"?>
<Project ToolsVersion="12.0" xmlns="http://schemas.microsoft.com/developer/msbuild/2003">
  <Import Project="../kaldi.props"/>
  <ItemGroup>'
    if ($libsrc) {
      echo ('    <LibSource Include="{0}" />' -f ($libsrc -join ";"))
    }
    if ($binsrc) {
      echo ('    <BinSource Include="{0}" />' -f ($binsrc -join ";"))
    }
    if ($testsrc) {
      echo ('    <TestSource Include="{0}" />' -f ($testsrc -join ";"))
    }
    if ($libdeps) {
      echo ('    <DependsOnLibs Include="{0}" />' -f ($libdeps -join ";"))
    }
    echo '  </ItemGroup>
  <Import Project="../kaldi.targets"/>
</Project>'
  }
}

# Pull all projects from the main makefile. $allproj is a map : string -> string array
# with all projects on the left (keys) and dependencies on the right. The set of dependencies
# is closed in respect to the membership in the set of keys.
write-host "Parsing main src/Makefile..."
$allproj = Get-Content "../src/Makefile"  | Unwrap-And-Decomment-Lines | ProcessMainMakefile

write-host "Crearting kaldi.kwproj..."
BuildMainProject $allproj | out-file "kaldi.kwproj" -encoding utf8

foreach ($proj in $allproj.Keys) {
  write-host "Converting ../src/$proj/Makefile -> $proj/kaldi-$proj.kwproj"
  mkdir -f $proj | out-null
  Get-Content "../src/$proj/Makefile"  | Unwrap-And-Decomment-Lines |
      BuildSubMakefile $proj | out-file "$proj/kaldi-$proj.kwproj" -encoding utf8
}

write-host @"
Conversion done.

1. Modify kaldi.user.props to configure.
2. Build by running (from MSVC or SDK command prompt)

   >msbuild -nologo -m -v:m

The default target is Test to build and test all libraries.
-t:Build only builds libraries and binaries, but does not build or run tests.
-t:Clean cleans everythig built.
"@
