import { useEffect, useState } from 'react';
import { api, type UserProfile } from './api';
import AuthModal from './components/AuthModal';
import PlayerModal from './components/PlayerModal';
import UploadView from './components/UploadView';
import VideoGrid from './components/VideoGrid';
import type { View } from './types';

function formatBytes(bytes?: number): string {
  if (bytes === undefined || bytes < 0) return '0 MB';
  if (bytes >= 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

export default function App() {
  const [user, setUser] = useState<UserProfile | null>(null);
  const [view, setView] = useState<View>('browse');
  const [showAuth, setShowAuth] = useState(false);
  const [playing, setPlaying] = useState<string | null>(null);
  const [reloadKey, setReloadKey] = useState(0);

  useEffect(() => {
    api.me()
      .then((profile) => setUser(profile.username ? profile : null))
      .catch(() => setUser(null));
  }, [reloadKey]);

  const go = (v: View) => {
    if (v !== 'browse' && !user?.username) {
      setShowAuth(true);
      return;
    }
    setView(v);
  };

  const reload = () => setReloadKey((k) => k + 1);

  const logout = async () => {
    await api.logout();
    setUser(null);
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
        <div style={{ display: 'flex', alignItems: 'center', gap: '0.75rem' }}>
          {user?.username ? (
            <>
              <span className="dim">
                {user.username}
                {typeof user.quota_bytes === 'number' && (
                  <> ({formatBytes(user.used_bytes)} / {formatBytes(user.quota_bytes)})</>
                )}
              </span>
              <button className="btn" onClick={() => void logout()}>Log out</button>
            </>
          ) : (
            <button className="btn red" onClick={() => setShowAuth(true)}>Log in</button>
          )}
        </div>
      </header>

      <main>
        {view === 'browse' && (
          <>
            <h2>Available videos</h2>
            <VideoGrid scope="all" reloadKey={reloadKey} onOpen={setPlaying} />
          </>
        )}
        {view === 'mine' && (
          <>
            <h2>My videos</h2>
            <VideoGrid scope="mine" reloadKey={reloadKey} onOpen={setPlaying} />
          </>
        )}
        {view === 'upload' && (
          <UploadView
            user={user}
            onUploaded={() => {
              reload();
              setView('mine');
            }}
          />
        )}
      </main>

      {showAuth && (
        <AuthModal
          onClose={() => setShowAuth(false)}
          onAuthed={(username) => {
            setUser({ username });
            setShowAuth(false);
            reload();
          }}
        />
      )}
      {playing && (
        <PlayerModal
          videoId={playing}
          onClose={() => {
            setPlaying(null);
            reload();
          }}
        />
      )}
    </>
  );
}