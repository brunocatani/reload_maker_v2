"""Fresh player workspace builds finished packs without game/CK/source setup."""
import argparse,json,os,subprocess,sys,time,uuid
from pathlib import Path
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT/'mcp'))
import server

def run(pack,application=None):
    work=ROOT/'local/home-tests'/uuid.uuid4().hex;workspace=work/'player';(workspace/'local/tmp').mkdir(parents=True)
    app=(application or ROOT/'build-fast/dist/ReloadMakerV2')/'ReloadMakerV2.exe'
    env=os.environ.copy();env['TEMP']=env['TMP']=str(workspace/'local/tmp');env.pop('ELRIC_EXE',None)
    server.ROOT=workspace;server._session=None;process=None
    def start():
        child=subprocess.Popen([str(app),'--project',str(workspace),'--no-restore'],env=env,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
        deadline=time.monotonic()+45
        while time.monotonic()<deadline:
            assert child.poll() is None,'Editor exited at first run'
            try:
                endpoint=json.loads((workspace/'local/settings/endpoint.json').read_text())
                if endpoint['pid']==child.pid:
                    state=server.command('state')
                    if not state['busy'] and server.command('home')['visible']:return child
            except (OSError,ValueError,RuntimeError):pass
            time.sleep(.1)
        child.terminate();child.wait(timeout=20);raise AssertionError('First-run home did not appear')
    try:
        process=start();initial=server.command('home');assert not initial['role'] and not initial['canResume']
        assert not initial['installation']['settings']['gameRoot']
        assert not initial['installation']['collisionReady']
        assert not server.command('state')['weapons']
        server.command('home',dict(role='player'))
        try:server.command('home',dict(role='invalid'))
        except RuntimeError:pass
        else:raise AssertionError('Unknown role accepted')
        process.terminate();process.wait(timeout=20);process=start()
        resumed=server.command('home');assert resumed['role']=='player' and resumed['visible']
        assert not resumed['playerNeedsCreationKit'] and not resumed['playerNeedsSourceScene'] and not server.command('state')['weapons']
        job=server.command('pack_job',dict(operation='build',packs=[str(pack)],installation=str(workspace/'installation'),initialize=True),server.command('state')['revision'])
        deadline=time.monotonic()+180
        while time.monotonic()<deadline:
            state=server.command('pack_job');assert state['id']==job['id']
            if state['phase'] in ('complete','failed'):break
            time.sleep(.2)
        assert state['phase']=='complete',state
        assert state['result']['activeObjects']>0 and (workspace/'installation/current.json').is_file()
        assert state['result']['runtimeCatalog']['ok'] and state['result']['runtimeCatalog']['objects']==state['result']['activeObjects']
        server.command('home',dict(role='creator'));assert server.command('home')['role']=='creator'
        assert not server.command('state')['weapons'],'Role choice silently loaded another source'
        (work/'result.json').write_text(json.dumps(dict(ok=True,initial=initial,resumed=resumed,build=state),indent=2))
        print('PASS fresh home, persistent roles and native pack build without game/Creation Kit:',work,flush=True)
    finally:
        if process is not None and process.poll() is None:
            process.terminate();process.wait(timeout=20)

if __name__=='__main__':
    parser=argparse.ArgumentParser();parser.add_argument('--pack',type=Path,required=True);parser.add_argument('--application',type=Path)
    args=parser.parse_args();run(args.pack.resolve(),args.application.resolve() if args.application else None)
