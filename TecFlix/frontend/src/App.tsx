import { useEffect, useState } from 'react';
import { api } from './api';
import AuthModal from './components/AuthModal';
import PlayerModal from './components/PlayerModal';
import UploadView from './components/UploadView';
import VideoGrid from './components/VideoGrid';
import type { View } from './types';

export default function App() {
  const [me, setMe] = useState<string | null>(null);
  const [view, setView] = useState<View>('browse');
  const [showAuth, setShowAuth] = useState(false);
  const [playing, setPlaying] = useState<string | null>(null);
  const [reloadKey, setReloadKey] = useState(0);

  useEffect(() => {
    api.me().then((r) => setMe(r.username)).catch(() => {});
  }, []);

  const go = (v: View) => {
    if (v !== 'browse' && !me) { setShowAuth(true); return; }
    setView(v);
  };
  const reload = () => setReloadKey((k) => k + 1);

  const logout = async () => {
    await api.logout();
    setMe(null);
    setView('browse');
    reload();
  };

  const tab = (v: View, label: string) => (
    <button className={view === v ? 'active' : ''} onClick={() => go(v)}>{label}</button>
  );

  return (
    <>
      <header>
        <div className="logo" onClick={() => go('browse')}>TECFLIX</div>
        <nav>
          {tab('browse', 'Browse')}
          {tab('mine', 'My Videos')}
          {tab('upload', 'Upload')}
        </nav>
        <div>
          {me ? (
            <>
              <span className="dim">{me} </span>
              <button className="btn" onClick={() => void logout()}>Log out</button>
            </>
          ) : (
            <button className="btn red" onClick={() => setShowAuth(true)}>Log in</button>
          )}
        </div>
      </header>

      <main>
        {view === 'browse' && (<><h2>Public videos</h2><VideoGrid scope="public" reloadKey={reloadKey} onOpen={setPlaying} /></>)}
        {view === 'mine' && (<><h2>My videos</h2><VideoGrid scope="mine" reloadKey={reloadKey} onOpen={setPlaying} /></>)}
        {view === 'upload' && <UploadView onUploaded={() => { reload(); setView('mine'); }} />}
      </main>

      {showAuth && (
        <AuthModal
          onClose={() => setShowAuth(false)}
          onAuthed={(u) => { setMe(u); setShowAuth(false); reload(); }}
        />
      )}
      {playing && <PlayerModal videoId={playing} onClose={() => { setPlaying(null); reload(); }} />}
    </>
  );
}
