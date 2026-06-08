require('dotenv').config();
const { GoogleGenerativeAI } = require("@google/generative-ai");

async function testGemini() {
    const apiKey = process.env.GEMINI_API_KEY;
    const modelName = process.env.GEMINI_MODEL || "gemini-1.5-flash";
    
    console.log(`Testing Gemini with Key: ${apiKey.substring(0, 5)}...`);
    console.log(`Model: ${modelName}`);

    const genAI = new GoogleGenerativeAI(apiKey);
    try {
        const model = genAI.getGenerativeModel({ model: modelName });
        const result = await model.generateContent("Hello, how are you?");
        console.log("Response Text:", result.response.text());
    } catch (error) {
        console.error("Gemini Test Error:", error);
    }
}

testGemini();
