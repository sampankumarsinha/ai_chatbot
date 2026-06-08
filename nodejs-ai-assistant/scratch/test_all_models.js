const { GoogleGenerativeAI } = require("@google/generative-ai");
require("dotenv").config();

async function testModels() {
  const genAI = new GoogleGenerativeAI(process.env.GEMINI_API_KEY);
  const modelsToTry = ["gemini-1.5-flash", "gemini-1.5-pro", "gemini-pro", "gemini-1.0-pro"];
  
  for (const modelName of modelsToTry) {
    try {
      console.log(`Testing model: ${modelName}...`);
      const model = genAI.getGenerativeModel({ model: modelName });
      const result = await model.generateContent("Hi");
      console.log(`✅ ${modelName} worked! Response: ${result.response.text().substring(0, 20)}...`);
      return modelName;
    } catch (error) {
      console.log(`❌ ${modelName} failed: ${error.message}`);
    }
  }
}

testModels();
