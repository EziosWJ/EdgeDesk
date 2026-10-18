import React from 'react';
import { createRoot } from 'react-dom/client';
import App from './app/App';
import './styles/edgedesk-tokens.css';
import './app.css';
import './desktop/desktop.css';

const root = document.getElementById('root');
if (!root) throw new Error('Missing #root');
createRoot(root).render(
  <React.StrictMode>
    <App />
  </React.StrictMode>,
);
