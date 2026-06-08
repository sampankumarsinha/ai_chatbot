import { GoogleGenerativeAI } from "@google/generative-ai";
import type { Channel, DefaultGenerics, Event, StreamChat } from "stream-chat";
import type { AIAgent } from "../types";

export class GeminiAgent implements AIAgent {
  private lastInteractionTs = Date.now();
  private readonly model: string;
  private readonly temperature: number;
  private readonly maxTokens: number;
  private readonly minFlushIntervalMs: number;
  private readonly minFlushChars: number;
  private static readonly fallbackModels = [
    "gemini-3-flash-preview",
    "gemini-3-pro-preview",
    "gemini-flash-latest",
  ];
  private genAI?: GoogleGenerativeAI;

  private readonly botUserId: string;

  constructor(
    readonly chatClient: StreamChat,
    readonly channel: Channel,
    model?: string
  ) {
    this.botUserId = `ai-bot-${channel.id?.replace(/[!]/g, "")}`;
    this.model = model || process.env.GEMINI_MODEL || "gemini-1.5-flash-latest";
    this.temperature = Number(process.env.GEMINI_TEMPERATURE || "0.4");
    this.maxTokens = Number(process.env.GEMINI_MAX_TOKENS || "220");
    this.minFlushIntervalMs = Number(process.env.GEMINI_STREAM_FLUSH_MS || "500");
    this.minFlushChars = Number(process.env.GEMINI_STREAM_MIN_CHARS || "24");
  }

  get user() {
    return this.chatClient.user;
  }

  getLastInteraction = (): number => this.lastInteractionTs;

  init = async () => {
    const apiKey = process.env.GEMINI_API_KEY;
    if (!apiKey) {
      throw new Error("GEMINI_API_KEY is required when AGENT_PLATFORM=gemini");
    }

    this.genAI = new GoogleGenerativeAI(apiKey);
  };

  dispose = async () => {
    // No socket listeners to detach anymore
  };

  processMessage = async (messageText: string) => {
    console.log(`[GeminiAgent] Processing message in channel: ${this.channel.cid}`);
    
    if (!this.genAI) {
      console.log(`[GeminiAgent] Agent not fully initialized`);
      return;
    }

    const message = messageText;
    console.log(`[GeminiAgent] Processing message: ${message}`);
    if (!message) {
      return;
    }

    this.lastInteractionTs = Date.now();

    const { message: channelMessage } = await this.channel.sendMessage({
      text: "",
      user_id: this.botUserId,
      ai_generated: true,
    });

    await this.channel.sendEvent({
      type: "ai_indicator.update",
      ai_state: "AI_STATE_THINKING",
      user_id: this.botUserId,
      cid: channelMessage.cid,
      message_id: channelMessage.id,
    });

    try {
      const stream = await this.generateStreamWithFallback(message);
      await this.channel.sendEvent({
        type: "ai_indicator.update",
        ai_state: "AI_STATE_GENERATING",
        user_id: this.botUserId,
        cid: channelMessage.cid,
        message_id: channelMessage.id,
      });

      let fullText = "";
      let lastPublished = "";
      let lastFlushTs = Date.now();

      try {
        for await (const chunk of stream.stream) {
          const delta = chunk.text();
          if (!delta) {
            continue;
          }

          fullText += delta;
          const now = Date.now();
          const newChars = fullText.length - lastPublished.length;
          const hitBoundary = /[.!?\n]\s*$/.test(fullText);
          const bySize = newChars >= this.minFlushChars;
          const byTime = now - lastFlushTs >= this.minFlushIntervalMs && newChars > 0;

          if (hitBoundary || bySize || byTime) {
            await this.chatClient.partialUpdateMessage(
              channelMessage.id,
              { set: { text: fullText } },
              this.botUserId
            );
            lastPublished = fullText;
            lastFlushTs = now;
          }
        }
      } catch (streamError) {
        console.error("[GeminiAgent] Stream processing error (non-fatal):", streamError);
        // We continue with whatever text we managed to collect
      }

      const finalText =
        fullText.trim() || "I could not generate a response. Please try again.";
      await this.chatClient.partialUpdateMessage(
        channelMessage.id,
        { set: { text: finalText } },
        this.botUserId
      );

      await this.channel.sendEvent({
        type: "ai_indicator.clear",
        user_id: this.botUserId,
        cid: channelMessage.cid,
        message_id: channelMessage.id,
      });
    } catch (error) {
      console.error("[GeminiAgent] Critical Error during processing:", error);
      let errorMessage =
        error instanceof Error ? error.message : "Gemini request failed";
      
      if (errorMessage.includes("429") || errorMessage.includes("Quota exceeded")) {
        errorMessage = "API quota exceeded. Please wait a minute and try again.";
      }

      await this.channel.sendEvent({
        type: "ai_indicator.update",
        ai_state: "AI_STATE_ERROR",
        user_id: this.botUserId,
        cid: channelMessage.cid,
        message_id: channelMessage.id,
      });

      await this.chatClient.partialUpdateMessage(
        channelMessage.id,
        { set: { text: `Gemini error: ${errorMessage}` } },
        this.botUserId
      );
    }
  };

  private generateStreamWithFallback = async (message: string) => {
    if (!this.genAI) {
      throw new Error("Gemini client is not initialized");
    }

    // 1. Fetch conversation history from the Stream channel
    const response = await this.channel.query({
      messages: { limit: 15 },
    });

    // 2. Map Stream messages to Gemini Content format
    // We filter for messages that have text and are not silent/system events
    const history = (response.messages || [])
      .filter((m) => m.text && m.type !== "system" && !m.shadowed)
      .map((m) => ({
        role: m.user?.id === this.botUserId ? "model" : "user",
        parts: [{ text: m.text || "" }],
      }));

    const modelCandidates = [
      this.model,
      ...GeminiAgent.fallbackModels.filter((model) => model !== this.model),
    ];

    let lastError: unknown;
    for (const modelName of modelCandidates) {
      try {
        const model = this.genAI.getGenerativeModel({
          model: modelName,
          systemInstruction: `You are a sophisticated AI Writing Assistant. Your goal is to help users create, refine, and improve their written content across various domains including Business, Creative Writing, Content Marketing, and Professional Communication.

Guidelines:
1. Tone: Professional yet encouraging, clear, and concise.
2. Structure: Use appropriate formatting (markdown) for better readability.
3. Quality: Focus on grammar, impact, and clarity.
4. If asked to brainstorm, provide a variety of creative options.
5. If asked to edit, explain the rationale for major changes.`,
          generationConfig: {
            maxOutputTokens: this.maxTokens,
            temperature: this.temperature,
          },
        });

        return await model.generateContentStream({
          contents: history,
        });
      } catch (error) {
        console.warn(`[GeminiAgent] Model ${modelName} failed, trying fallback...`, error);
        lastError = error;
      }
    }

    throw lastError instanceof Error
      ? lastError
      : new Error("No compatible Gemini model found for this API key");
  };
}
