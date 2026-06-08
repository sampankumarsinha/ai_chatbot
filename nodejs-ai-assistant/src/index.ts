import cors from "cors";
import compression from "compression";
import "dotenv/config";
import express from "express";
import { apiKey, serverClient } from "./serverClient";
import { GeminiAgent } from "./agents/gemini/GeminiAgent";

const app = express();

const agents = new Map<string, any>();

app.use(compression());
app.use(express.json({ limit: "50kb" }));

const allowedOrigin = process.env.ALLOWED_ORIGIN || "*";

app.use(
  cors({
    origin: true, // Reflects the request origin
    credentials: true,
  })
);

// ================= BASIC HEALTH =================
app.get("/", (req, res) => {
  res.json({
    message: `AI Server running (${process.env.AGENT_PLATFORM || "n8n"} mode)`,
    apiKey,
    platform: process.env.AGENT_PLATFORM || "n8n",
  });
});

// ================= START AI =================
app.post("/start-ai-agent", async (req, res) => {
  const { channel_id, channel_type = "messaging" } = req.body;

  if (!channel_id) {
    return res.status(400).json({ error: "Missing channel_id" });
  }

  const user_id = `ai-bot-${channel_id.replace(/[!]/g, "")}`;

  try {
    await serverClient.upsertUser({
      id: user_id,
      name: "AI Assistant",
    });

    const channel = serverClient.channel(channel_type, channel_id);
    await channel.create(); // Ensure the channel exists before adding members
    await channel.addMembers([user_id]);

    // Initialize agent if platform is gemini
    if (process.env.AGENT_PLATFORM === "gemini") {
      if (!agents.has(channel_id)) {
        console.log(`Initializing GeminiAgent for channel: ${channel_id}`);
        const agent = new GeminiAgent(serverClient, channel);
        await agent.init();
        agents.set(channel_id, agent);
      }
    }

    res.json({ status: "connected" });
  } catch (error) {
    console.error("Failed to start AI agent:", error);
    res.status(500).json({ error: "Failed to start AI agent" });
  }
});

// ================= INJECT MESSAGE =================
app.post("/inject-ai-message", async (req, res) => {
  const { channel_id, text, channel_type = "messaging" } = req.body;

  if (!channel_id || !text) {
    return res.status(400).json({ error: "Missing fields" });
  }

  const user_id = `ai-bot-${channel_id.replace(/[!]/g, "")}`;

  try {
    const channel = serverClient.channel(channel_type, channel_id);

    await channel.sendMessage({
      text,
      user_id,
    });

    res.json({ success: true });
  } catch {
    res.status(500).json({ error: "Failed to send message" });
  }
});

// ================= STOP AI =================
app.post("/stop-ai-agent", async (req, res) => {
  const { channel_id } = req.body;

  if (!channel_id) {
    return res.status(400).json({ error: "Missing channel_id" });
  }

  const agent = agents.get(channel_id);
  if (agent) {
    console.log(`Stopping agent for channel: ${channel_id}`);
    await agent.dispose();
    agents.delete(channel_id);
  }

  res.json({ status: "stopped" });
});

// ================= PROXY TO N8N =================
app.post("/proxy-n8n", async (req, res) => {
  const { message } = req.body;

  const webhookUrl =
    process.env.N8N_WEBHOOK_URL ||
    "https://sampansinha.app.n8n.cloud/webhook/8864f904-0e10-47cb-928e-5b9f675e7833";

  if (!message) {
    return res.status(400).json({ error: "Missing message" });
  }

  try {
    const response = await fetch(webhookUrl, {
      method: "POST",
      headers: {
        "Content-Type": "application/json",
      },
      body: JSON.stringify({ message }),
    });

    const raw = await response.text();

    if (!response.ok) {
      return res.status(response.status).json({
        error: "n8n failed",
        details: raw,
      });
    }

    let data;

    try {
      data = JSON.parse(raw);
    } catch {
      return res.status(500).json({
        error: "Invalid JSON from n8n",
        raw,
      });
    }

    return res.json({
      reply: data.reply || "No reply",
    });
  } catch (error) {
    return res.status(500).json({
      error: "Proxy failed",
      reason: error instanceof Error ? error.message : String(error),
    });
  }
});

// ================= PROCESS MESSAGE (Trigger AI) =================
app.post("/process-message", async (req, res) => {
  const { channel_id, message } = req.body;

  if (!channel_id || !message) {
    return res.status(400).json({ error: "Missing channel_id or message" });
  }

  try {
    let agent = agents.get(channel_id);
    
    // Auto-initialize agent if missing (e.g. after server restart)
    if (!agent && process.env.AGENT_PLATFORM === "gemini") {
      console.log(`Auto-initializing GeminiAgent for channel: ${channel_id}`);
      const channel = serverClient.channel("messaging", channel_id);
      agent = new GeminiAgent(serverClient, channel);
      await agent.init();
      agents.set(channel_id, agent);
    }

    if (agent && typeof agent.processMessage === "function") {
      // Trigger processing in background
      agent.processMessage(message).catch((err: any) => {
        console.error("Async Process Message Error:", err);
      });
      return res.json({ success: true, triggered: true });
    }

    res.status(404).json({ error: "Agent not found or platform not supported for auto-trigger" });
  } catch (error) {
    console.error("Process Message Trigger Error:", error);
    res.status(500).json({ error: "Failed to trigger AI" });
  }
});

// ================= STATUS =================
app.get("/agent-status", (req, res) => {
  const { channel_id } = req.query;
  if (channel_id && typeof channel_id === "string") {
    const isConnected = agents.has(channel_id);
    return res.json({ status: isConnected ? "connected" : "disconnected" });
  }
  res.json({ status: "connected" });
});

// ================= TOKEN =================
app.post("/token", (req, res) => {
  const { userId } = req.body;

  if (!userId) {
    return res.status(400).json({ error: "userId required" });
  }

  const issuedAt = Math.floor(Date.now() / 1000);
  const expiration = issuedAt + 60 * 60;

  const token = serverClient.createToken(userId, expiration, issuedAt);

  res.json({ token });
});

// ================= START SERVER =================
const port = process.env.PORT || 3000;

app.listen(port, () => {
  console.log(`Server running on http://localhost:${port}`);
});
