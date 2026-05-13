/**
 * InferDeck - Local AI Control Plane (React + Tailwind + Vite)
 * Main entry point
 */

import { StrictMode } from 'react'
import { createRoot } from 'react-dom/client'
import './styles/globals.css'
import App from './App'

const root = createRoot(document.getElementById('root')!)
root.render(
  <StrictMode>
    <App />
  </StrictMode>
)
