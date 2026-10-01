"""Exercise relocated bundled MCP without developer programs on PATH."""
import argparse
import asyncio
import json
import os
from pathlib import Path
from mcp import ClientSession, StdioServerParameters
from mcp.client.stdio import stdio_client


async def run(args):
    application=Path(args.application).resolve()
    workspace=Path(args.workspace).resolve()
    environment=dict(os.environ, PATH=str(Path(os.environ['SystemRoot'])/'System32'),
                     TEMP=str(workspace/'local/tmp'), TMP=str(workspace/'local/tmp'))
    for adapter, tool, inputs in (
        ('studio','get_state',{}),
        ('nif','nif_info',{'path':str(Path(args.nif).resolve())}),
        ('archive','bsa_status',{}),
    ):
        parameters=StdioServerParameters(command=str(application/'runtime/python/python.exe'),
            args=['-B','-I',str(application/'tools/product_cli.py'),'--workspace',str(workspace),'mcp','--adapter',adapter],env=environment)
        async with stdio_client(parameters) as (reader,writer):
            async with ClientSession(reader,writer) as client:
                await client.initialize()
                result=await client.call_tool(tool,inputs)
                assert not result.isError,(adapter,result)
                value=json.loads(next(c.text for c in result.content if c.type=='text'))
                assert value.get('ok', adapter=='studio'),(adapter,value)
                print(json.dumps(dict(adapter=adapter,ok=True)))


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--application',required=True)
    parser.add_argument('--workspace',required=True)
    parser.add_argument('--nif',required=True)
    asyncio.run(run(parser.parse_args()))
