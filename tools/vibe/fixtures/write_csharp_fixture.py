"""A .csproj with one error and one warning in it, written beside a sandbox.

The point of the fixture is that MSBuild states the answer itself: `1 Warning(s)`
and `1 Error(s)` land in the same `raw_output` the tool returns, so the tool's
own `diagnostics_count` has a control sitting next to it in the payload rather
than in this file. Kept out of `sandbox.py` because a C# project changes what
Godot does with the directory and every other probe wants the GDScript one.
"""

from __future__ import annotations

import sys
from pathlib import Path

CSPROJ = """<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <TargetFramework>net8.0</TargetFramework>
    <EnableDefaultCompileItems>false</EnableDefaultCompileItems>
  </PropertyGroup>
  <ItemGroup><Compile Include="Player.cs" /></ItemGroup>
</Project>
"""

# `Healht` is one transposition, which is CS0103; `notUsed` is CS0219. One of
# each severity, on known lines, so a position report can be checked too.
PLAYER = """public class Player
{
    public int Health = 100;

    public void Damage(int amount)
    {
        Healht -= amount;
    }

    public void Unused()
    {
        int notUsed = 5;
    }
}
"""


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: write_csharp_fixture.py SANDBOX")
        return 2
    root = Path(sys.argv[1])
    (root / "Game.csproj").write_text(CSPROJ)
    (root / "Player.cs").write_text(PLAYER)
    print(f"wrote {root / 'Game.csproj'} and {root / 'Player.cs'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
