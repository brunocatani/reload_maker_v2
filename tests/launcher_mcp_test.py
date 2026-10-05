"""The repository launcher uses the shipped runtime with developer PATH removed."""
import asyncio,json,os
from pathlib import Path
from mcp import ClientSession,StdioServerParameters
from mcp.client.stdio import stdio_client
ROOT=Path(__file__).resolve().parents[1]

async def run():
    system=Path(os.environ['SystemRoot'])/'System32'
    env=dict(os.environ,PATH=str(system),TEMP=str(ROOT/'local/tmp'),TMP=str(ROOT/'local/tmp'))
    parameters=StdioServerParameters(command=str(system/'WindowsPowerShell/v1.0/powershell.exe'),
        args=['-NoProfile','-ExecutionPolicy','Bypass','-File',str(ROOT/'tools/Start-ReloadMakerV2.ps1'),'-Mcp'],env=env)
    async with stdio_client(parameters) as (reader,writer):
        async with ClientSession(reader,writer) as client:
            await client.initialize();catalog=await client.list_tools()
            assert {'home','get_pack_coverage','checklist','configure_magazine_presentation'}<={t.name for t in catalog.tools}
            result=await client.call_tool('home',{});assert not result.isError,result
            value=json.loads(next(c.text for c in result.content if c.type=='text'));assert Path(value['workspace'])==ROOT
            print('PASS bundled launcher runtime and current studio capability catalog with developer PATH removed')

if __name__=='__main__':asyncio.run(run())
