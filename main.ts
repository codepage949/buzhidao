import { loadSync } from "dotenv";

loadSync({ export: true, allowEmptyValues: true });

import * as io from "io";
import * as clipboard from "clipboard-image";
import imgScr from "imgScript";
import OpenAI from "openai";
import { groupDetections } from "./src/detection.ts";
import { TelegramClient, type TelegramUpdate } from "./src/telegram.ts";

const user32 = Deno.dlopen(
  "user32.dll",
  {
    PeekMessageA: {
      parameters: ["buffer", "usize", "u32", "u32", "u32"],
      result: "i32",
      nonblocking: false,
    },
    SetWindowsHookExA: {
      parameters: ["i32", "function", "usize", "u32"],
      result: "i32",
      nonblocking: false,
    },
    CallNextHookEx: {
      parameters: ["usize", "i32", "usize", "usize"],
      result: "i32",
      nonblocking: false,
    },
    SendInput: {
      parameters: ["u32", "buffer", "i32"],
      result: "i32",
      nonblocking: false,
    },
  } as const,
);
let isPrtScPressed = false;
let isBusy = false;
const callbackDataMap = new Map<string, string>();
const sourceLanguage = requireEnv("SOURCE");
const apiBaseUrl = requireEnv("API_BASE_URL");
const chatId = Deno.env.get("CHAT_ID");
const xDelta = parseInt(requireEnv("X_DELTA"));
const yDelta = parseInt(requireEnv("Y_DELTA"));
const telegram = new TelegramClient({
  apiBaseUrl: requireEnv("TELEGRAM_API_BASE_URL"),
  botToken: requireEnv("BOT_TOKEN"),
});
const keyboardHook = new Deno.UnsafeCallback(
  { parameters: ["i32", "usize", "usize"], result: "usize" } as const,
  (
    nCode: number,
    wParam: bigint,
    lParam: bigint,
  ): bigint => {
    const keyCode =
      (new Deno.UnsafePointerView(Deno.UnsafePointer.create(lParam)!))
        .getUint32();
    const flags =
      (new Deno.UnsafePointerView(Deno.UnsafePointer.create(lParam)!))
        .getUint32(8);

    if (nCode === 0 && (flags & 0x10) === 0) {
      if (wParam === 257n) {
        if (keyCode === 0x2C) {
          // NOTE: lParam 포인터의 데이터를 조작하여 alt키를 삽입할 수도 있을지는 모르겠으나
          //     아직 deno에서 포인터를 통한 데이터 쓰기를 지원하지 않아 sendInput으로 대체
          const input = new DataView(new Uint8Array(160).buffer);

          input.setUint32(0, 1, true);
          input.setUint16(8, 0x12, true);

          input.setUint32(40 + 0, 1, true);
          input.setUint16(40 + 8, 0x2C, true);

          input.setUint32(80 + 0, 1, true);
          input.setUint16(80 + 8, 0x2C, true);
          input.setUint32(80 + 12, 2, true);

          input.setUint32(120 + 0, 1, true);
          input.setUint16(120 + 8, 0x12, true);
          input.setUint32(120 + 12, 2, true);
          user32.symbols.SendInput(4, new Uint8Array(input.buffer), 40);

          isPrtScPressed = true;

          return 1n;
        }
      } else {
        if (keyCode === 0x2C) {
          return 1n;
        }
      }
    }

    return user32.symbols.CallNextHookEx(
      0n,
      nCode,
      wParam,
      lParam,
    ) as unknown as bigint;
  },
);

const client = new OpenAI({
  apiKey: requireEnv("AI_GATEWAY_API_KEY"),
  baseURL: "https://ai-gateway.vercel.sh/v1",
});

function requireEnv(name: string): string {
  const value = Deno.env.get(name);

  if (!value) {
    throw new Error(`Missing environment variable: ${name}`);
  }

  return value;
}

async function makeMessage(txts: string[]) {
  const joinedTxt = txts.join("\n");
  const systemPromptPath = requireEnv("SYSTEM_PROMPT_PATH");
  const systemPrompt = await Deno.readTextFile(systemPromptPath);
  const response = await client.chat.completions.create({
    model: requireEnv("AI_GATEWAY_MODEL"),
    messages: [
      {
        role: "system",
        content: systemPrompt,
      },
      {
        role: "user",
        content: joinedTxt,
      },
    ],
    temperature: 0.7,
  });

  return response.choices[0].message.content ?? "";
}

async function* infinity() {
  while (true) {
    yield;

    await new Promise((ok) => setTimeout(ok, 0));
  }
}

async function pollTgMessage() {
  let offset = 0;

  for await (const _ of infinity()) {
    const updates = await telegram.getUpdates(offset);

    for (const result of updates) {
      await handleTelegramUpdate(result);

      offset = result.update_id + 1;
    }

    await new Promise((ok) => {
      setTimeout(ok, 500);
    });
  }
}

function setKeyboardHook() {
  user32.symbols.SetWindowsHookExA(13, keyboardHook.pointer, 0n, 0);
}

async function handleTelegramUpdate(result: TelegramUpdate) {
  if (result.callback_query) {
    const txt = callbackDataMap.get(result.callback_query.data);

    if (txt && chatId) {
      const message = await makeMessage([txt]);
      await telegram.sendMessage({
        chat_id: chatId,
        text: message,
      });
    }

    await telegram.answerCallbackQuery(result.callback_query.id);
    return;
  }

  if (!result.message?.text) {
    return;
  }

  const targetChatId = chatId ?? `${result.message.chat.id}`;
  const message = chatId
    ? await makeMessage([result.message.text])
    : `${result.message.chat.id}`;

  await telegram.sendMessage({
    chat_id: targetChatId,
    text: message,
  });
}

async function detectTextsFromClipboard(): Promise<string[]> {
  const bmpImg = await io.readAll(await clipboard.read());
  const img = await imgScr.decode(bmpImg);

  if (img.width > 1024) {
    const ratio = 1024 / img.width;
    img.resize(1024, Math.round(img.height * ratio));
  }

  const pngImg = await img.encode(1);

  await (await Deno.open("output.png", {
    create: true,
    write: true,
    truncate: true,
  })).write(pngImg);

  const fd = new FormData();
  fd.set(
    "file",
    new File([await Deno.readFile("output.png")], "output.png"),
  );

  const response = await fetch(`${apiBaseUrl}/infer/${sourceLanguage}`, {
    method: "post",
    body: fd,
  });

  console.log(`${apiBaseUrl}/infer/${sourceLanguage}`, response.status);

  const detections = await response.json();
  return groupDetections(detections, sourceLanguage, xDelta, yDelta);
}

async function sendDetectionResult(txts: string[]) {
  if (!chatId) {
    return;
  }

  if (txts.length < 4) {
    const message = txts.length > 0
      ? await makeMessage(txts)
      : "no detections.";
    await telegram.sendMessage({
      chat_id: chatId,
      text: message,
      parse_mode: "Markdown",
    });
    return;
  }

  const inline_keyboard = [];

  for (const txt of txts) {
    const id = crypto.randomUUID();
    callbackDataMap.set(id, txt);
    inline_keyboard.push([{ text: txt, callback_data: id }]);
  }

  await telegram.sendMessage({
    chat_id: chatId,
    text: "번역할 텍스트를 선택해주세요.",
    reply_markup: { inline_keyboard },
  });
}

async function pumpWsMessage() {
  const msg = new Uint8Array(48);

  for await (const _ of infinity()) {
    user32.symbols.PeekMessageA(msg, 0n, 0, 0, 1);

    if (isPrtScPressed) {
      isPrtScPressed = false;

      if (!isBusy) {
        isBusy = true;

        try {
          await new Promise((ok) => setTimeout(ok, 500));
          const txts = await detectTextsFromClipboard();
          await sendDetectionResult(txts);
        } catch (e) {
          console.log(e);
        } finally {
          isBusy = false;
        }
      }
    }
  }
}

(function main() {
  pollTgMessage();
  setKeyboardHook();
  pumpWsMessage();
})();
