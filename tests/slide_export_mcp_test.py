"""Verify the consumer export against the complete recipe via the real MCP server."""
import asyncio
import json
import os
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client

ROOT = Path(__file__).resolve().parents[1]


async def run():
    params = StdioServerParameters(command='py', args=['-3.14', str(ROOT/'mcp/server.py')],
        env=dict(os.environ, TEMP=str(ROOT/'local/tmp'), TMP=str(ROOT/'local/tmp'),
                 PYTHONPYCACHEPREFIX=str(ROOT/'local/pycache')))
    async with stdio_client(params) as (read, write):
        async with ClientSession(read, write) as session:
            await session.initialize()

            async def call(name, **arguments):
                result = await session.call_tool(name, arguments)
                assert not result.isError, result
                return json.loads(next(c.text for c in result.content if c.type == 'text'))

            tools = await session.list_tools()
            assert 'export_glock_runtime_slides' in {t.name for t in tools.tools}
            before = await call('get_state')
            assert not before['busy'] and not before['lastError']
            simulation = await call('export_authored_record', kind='simulation', expected_revision=before['revision'])
            slide = await call('export_authored_record', kind='slide', expected_revision=before['revision'])
            full = json.loads(Path(simulation['path']).read_text(encoding='utf-8'))
            compact = json.loads(Path(slide['path']).read_text(encoding='utf-8'))
            assert compact['framePath'] == 'Weapon' and compact['recipeId'] == full['recipeId']
            assert len(compact['tracks']) == len(full['slideTracks'])
            first, last = full['openFrame'], full['closedFrame']
            for track, original in zip(compact['tracks'], full['slideTracks']):
                source = full['sourceRecipe']['tracks'][original['track']]
                assert track['anchor'] == source['anchor']
                assert track['poses'] == source['poses'][first:last+1], 'A source sample changed or disappeared'
                assert track['runtimePath'].startswith('Weapon/') and track['runtimePath'].endswith('/'+track['anchor']['name'])
            after = await call('get_state')
            for key in ('project', 'slots', 'magazineGrabGrips', 'camera'):
                assert before[key] == after[key], 'Export changed authoring state: '+key
            print('PASS: real MCP slide export preserves all original samples, rig identity and authoring state')


asyncio.run(run())
