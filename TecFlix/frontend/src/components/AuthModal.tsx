import { useState, type FormEvent } from 'react';
import { api } from '../api';
import Modal from './Modal';

interface Props {
  onClose: () => void;
  onAuthed: (username: string) => void;
}

export default function AuthModal({ onClose, onAuthed }: Props) {
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [error, setError] = useState('');

  const submit = async (kind: 'login' | 'register') => {
    setError('');
    try {
      const r = await api[kind](username, password);
      onAuthed(r.username);
    } catch (e) {
      setError(e instanceof Error ? e.message : 'Something went wrong');
    }
  };

  return (
    <Modal onClose={onClose} small>
      <h2>Sign in</h2>
      <form onSubmit={(e: FormEvent) => { e.preventDefault(); void submit('login'); }}>
        <label>Username</label>
        <input type="text" value={username} autoComplete="username" autoFocus onChange={(e) => setUsername(e.target.value)} />
        <label>Password</label>
        <input type="password" value={password} autoComplete="current-password" onChange={(e) => setPassword(e.target.value)} />
        <div className="err">{error}</div>
        <div className="row">
          <button type="submit" className="btn red">Log in</button>
          <button type="button" className="btn" onClick={() => void submit('register')}>Create account</button>
        </div>
      </form>
    </Modal>
  );
}
