Deploy checklist for chat-ai-app

Overview
- Frontend: `react-stream-ai-assistant` — deploy to Vercel (edge CDN)
- Backend: `nodejs-ai-assistant` — deploy to Render (recommended). Render supports long-running Node processes and environment variables.

Frontend (Vercel)
1. Create a Vercel project and connect the repo.
2. Set root/path to `react-stream-ai-assistant` if using monorepo.
3. Build settings:
   - Framework: Vite (auto-detect)
   - Build command: `npm run build`
   - Output directory: `dist`
4. Environment variables (Vercel Project Settings):
   - `VITE_STREAM_API_KEY` = <your stream api key>
   - `VITE_BACKEND_URL` = https://<your-backend-host>
5. Deploy and verify the site loads.

Backend (Render)
1. Create a new Web Service on Render and connect to repo.
2. Set the service root to `nodejs-ai-assistant`.
3. Build & Start commands:
   - Build command: `npm run build`
   - Start command: `npm start`
4. Environment variables (Render Dashboard):
   - `STREAM_API_KEY`
   - `STREAM_API_SECRET`
   - `OPENAI_API_KEY` (or Anthropic)
   - `TAVILY_API_KEY`
   - `AGENT_PLATFORM` (optional)
   - `ALLOWED_ORIGIN` = https://<your-frontend-host>
5. Instance type: choose a single instance with enough memory (512MB+), Node 20+ runtime.
6. Deploy and confirm the health:
   - GET / should return a JSON message
   - POST /token with body {"userId":"test"} should return a token

Notes
- For production safety, ensure `ALLOWED_ORIGIN` points to your frontend URL.
- If you need horizontal scaling, move in-memory agent state to Redis.

Smoke tests (local)
```bash
# Frontend
cd react-stream-ai-assistant
npm install
npm run build
npm run preview

# Backend
cd nodejs-ai-assistant
npm install
npm run start

# Smoke tests
curl -i http://localhost:3000/
curl -i 'http://localhost:3000/agent-status?channel_id=test'
curl -X POST -H 'Content-Type: application/json' -d '{"userId":"test"}' http://localhost:3000/token
```
