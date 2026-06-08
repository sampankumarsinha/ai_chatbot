const { GoogleGenerativeAI } = require("@google/generative-ai");
require('dotenv').config();

async function listModels() {
    try {
        const genAI = new GoogleGenerativeAI(process.env.GEMINI_API_KEY);
        console.log("Using Key ending in:", process.env.GEMINI_API_KEY.slice(-4));
        
        // Use the native fetch to list models or the SDK if possible
        // Actually the SDK doesn't have a direct listModels, we use the REST API
        const response = await fetch(`https://generativelanguage.googleapis.com/v1beta/models?key=${process.env.GEMINI_API_KEY}`);
        const data = await response.json();
        
        if (data.models) {
            console.log("Available Models:");
            data.models.forEach(m => console.log("- " + m.name.replace('models/', '')));
        } else {
            console.log("No models found or error in response:", JSON.stringify(data));
        }
    } catch (error) {
        console.error("Error listing models:", error);
    }
}

listModels();
