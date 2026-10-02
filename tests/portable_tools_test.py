"""Exercise relocated bundled MCP without developer programs on PATH."""
import argparse
import asyncio
import json
import os
import uuid
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def run(args):
    application=Path(args.application).resolve()
    workspace=Path(args.workspace).resolve()
    extraction=workspace/'local/portable-archive'/uuid.uuid4().hex
    environment=dict(os.environ, PATH=str(Path(os.environ['SystemRoot'])/'System32'),
                     TEMP=str(workspace/'local/tmp'), TMP=str(workspace/'local/tmp'))
    operations = [
        ('studio','get_state',{}),
        ('nif','nif_info',{'path':str(Path(args.nif).resolve())}),
        ('archive','bsa_status',{}),
    ]
    if args.archive:
        selector=dict(archive_path=str(Path(args.archive).resolve()),
                      include_filters=[r'*\_1stperson\animations\pistol\wpnidlereadya.hkx'])
        operations.extend([
            ('archive','bsa_list',dict(selector,max_entries=4)),
            ('archive','bsa_extract',dict(selector,destination_path=str(extraction),max_processed_files=4)),
        ])
    for adapter, tool, inputs in operations:
        parameters=StdioServerParameters(command=str(application/'runtime/python/python.exe'),
            args=['-B','-I',str(application/'tools/product_cli.py'),'--workspace',str(workspace),'mcp','--adapter',adapter],env=environment)
        async with stdio_client(parameters) as (reader,writer):
            async with ClientSession(reader,writer) as client:
                await client.initialize()
                result=await client.call_tool(tool,inputs)
                assert not result.isError,(adapter,result)
                value=json.loads(next(c.text for c in result.content if c.type=='text'))
                assert value.get('ok', adapter=='studio'),(adapter,value)
                if tool=='bsa_list':assert value['entryCount']==1,value
                if tool=='bsa_extract':
                    files=list(extraction.rglob('*.hkx'));assert len(files)==1 and files[0].stat().st_size>1000
                print(json.dumps(dict(adapter=adapter,tool=tool,ok=True)))


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--application',required=True)
    parser.add_argument('--workspace',required=True)
    parser.add_argument('--nif',required=True)
    parser.add_argument('--archive')
    asyncio.run(run(parser.parse_args()))
