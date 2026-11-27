import { useCallback, useEffect, useState, type FormEvent } from 'react';
import { api, type AdminUser, type AdminVideo, type UserProfile } from '../api';
import UploadView from './UploadView';

type Tab = 'users' | 'videos' | 'add-video';

function formatBytes(bytes: number): string {
  if (bytes >= 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
  return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
}

const formatDate = (unix: number) => (unix ? new Date(unix * 1000).toLocaleString() : '—');

export default function AdminDashboard({ user }: { user: UserProfile }) {
  const [tab, setTab] = useState<Tab>('users');
  const [users, setUsers] = useState<AdminUser[]>([]);
  const [videos, setVideos] = useState<AdminVideo[]>([]);
  const [newUser, setNewUser] = useState('');
  const [newPass, setNewPass] = useState('');
  const [error, setError] = useState('');
  const [info, setInfo] = useState('');

  const load = useCallback(async () => {
    try {
      const [u, v] = await Promise.all([api.admin.users(), api.admin.videos()]);
      setUsers(u);
      setVideos(v);
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Failed to load admin data');
    }
  }, []);

  useEffect(() => { void load(); }, [load]);

  const run = async (fn: () => Promise<string | void>) => {
    setError('');
    setInfo('');
    try {
      const msg = await fn();
      if (msg) setInfo(msg);
      await load();
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Operation failed');
    }
  };

  const addUser = (e: FormEvent) => {
    e.preventDefault();
    void run(async () => {
      await api.admin.createUser(newUser, newPass);
      setNewUser('');
      setNewPass('');
      return 'User created.';
    });
  };

  const removeUser = (name: string) => {
    if (!confirm(`Delete user "${name}" and all of their videos? This cannot be undone.`)) return;
    void run(async () => {
      const r = await api.admin.deleteUser(name);
      return `User deleted (${r.videosDeleted} video(s) removed).`;
    });
  };

  const removeVideo = (v: AdminVideo) => {
    if (!confirm(`Delete video "${v.title}"? This cannot be undone.`)) return;
    void run(async () => {
      await api.admin.deleteVideo(v.id);
      return 'Video deleted.';
    });
  };

  const totalBytes = videos.reduce((a, v) => a + v.sizeBytes, 0);

  return (
    <>
      <h2>Admin dashboard</h2>
      <div className="admin-stats">
        <div className="stat"><b>{users.length}</b><span className="dim">Registered users</span></div>
        <div className="stat"><b>{videos.length}</b><span className="dim">Videos</span></div>
        <div className="stat"><b>{formatBytes(totalBytes)}</b><span className="dim">Total storage</span></div>
      </div>

      <div className="admin-tabs">
        <button className={'btn' + (tab === 'users' ? ' active' : '')} onClick={() => setTab('users')}>Users</button>
        <button className={'btn' + (tab === 'videos' ? ' active' : '')} onClick={() => setTab('videos')}>Videos</button>
        <button className={'btn' + (tab === 'add-video' ? ' active' : '')} onClick={() => setTab('add-video')}>Add video</button>
      </div>

      <div className="err">{error}</div>
      <div className="ok">{info}</div>

      {tab === 'users' && (
        <>
          <form className="admin-form" onSubmit={addUser}>
            <div>
              <label>New username</label>
              <input type="text" value={newUser} onChange={(e) => setNewUser(e.target.value)} />
            </div>
            <div>
              <label>Password</label>
              <input type="password" value={newPass} autoComplete="new-password" onChange={(e) => setNewPass(e.target.value)} />
            </div>
            <button type="submit" className="btn red">Add user</button>
          </form>
          <table className="admin-table">
            <thead>
              <tr><th>Username</th><th>Created</th><th>Storage used</th><th>Videos</th><th></th></tr>
            </thead>
            <tbody>
              {users.map((u) => (
                <tr key={u.username}>
                  <td>{u.username}{u.isAdmin && <span className="badge">admin</span>}</td>
                  <td>{formatDate(u.created_at)}</td>
                  <td>{formatBytes(u.used_bytes)} / {formatBytes(u.quota_bytes)}</td>
                  <td>{videos.filter((v) => v.owner === u.username).length}</td>
                  <td>
                    {!u.isAdmin && (
                      <button className="btn danger" onClick={() => removeUser(u.username)}>Delete</button>
                    )}
                  </td>
                </tr>
              ))}
            </tbody>
          </table>
          {users.length === 0 && <div className="empty">No users.</div>}
        </>
      )}

      {tab === 'videos' && (
        <>
          <table className="admin-table">
            <thead>
              <tr><th>Title</th><th>Owner</th><th>Visibility</th><th>Size</th><th>Uploaded</th><th></th></tr>
            </thead>
            <tbody>
              {videos.map((v) => (
                <tr key={v.id}>
                  <td>{v.title}</td>
                  <td>{v.owner}</td>
                  <td><span className={'badge ' + v.visibility}>{v.visibility}</span></td>
                  <td>{formatBytes(v.sizeBytes)}</td>
                  <td>{formatDate(v.uploaded)}</td>
                  <td><button className="btn danger" onClick={() => removeVideo(v)}>Delete</button></td>
                </tr>
              ))}
            </tbody>
          </table>
          {videos.length === 0 && <div className="empty">No videos.</div>}
        </>
      )}

      {tab === 'add-video' && (
        <UploadView
          user={user}
          onUploaded={() => {
            setInfo('Video uploaded.');
            setTab('videos');
            void load();
          }}
        />
      )}
    </>
  );
}
