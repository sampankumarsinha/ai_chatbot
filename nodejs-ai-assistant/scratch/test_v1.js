require("dotenv").config();

async function testV1() {
  const url = `https://generativelanguage.googleapis.com/v1/models/gemini-1.5-flash:generateContent?key=${process.env.GEMINI_API_KEY}`;
  
  try {
    console.log("Testing V1 API directly via global fetch...");
    const response = await fetch(url, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({
        contents: [{ parts: [{ text: "Hi" }] }]
      })
    });
    const data = await response.json();
    console.log("Response Status:", response.status);
    console.log("Response:", JSON.stringify(data, null, 2));
  } catch (error) {
    console.error("Fetch Error:", error);
  }
}

testV1();
