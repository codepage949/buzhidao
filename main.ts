import { config } from "dotenv";
import cryptoJs from "crypto-js";
import pinyin from "pinyin";

config({ export: true });

async function renewPapagoVersion() {
  const mainJsName = await (async function getMainJsName() {
    const resp = await fetch("https://papago.naver.com");
    const source = await resp.text();
    const start = source.indexOf('src="/main') + 5;
    const end = source.indexOf('"', start);

    return source.substring(start, end);
  })();

  const papagoVersion = await (async function getPapagoVersion(mainJsName) {
    const resp = await fetch(`https://papago.naver.com${mainJsName}`);
    const source = await resp.text();
    const start = source.indexOf("HmacMD5");
    const end = source.indexOf('").toS', start);

    return source.substring(start, end).split(',"')[1];
  })(mainJsName);

  return papagoVersion;
}

let papagoVersion = await renewPapagoVersion();

async function translate(text: string) {
  const uuid = crypto.randomUUID();
  const path = Deno.env.get("PAPAGO_API_URL");
  const timestamp = new Date().valueOf().toString();
  const auth = cryptoJs.enc.Base64.stringify(
    cryptoJs.HmacMD5(
      `${uuid}\n${path}\n${timestamp}`,
      papagoVersion,
    ),
  );
  const usp = new URLSearchParams();

  usp.set("deviceId", uuid);
  usp.set("locale", "en");
  usp.set("dict", "true");
  usp.set("dictDisplay", "30");
  usp.set("honorific", "false");
  usp.set("instant", "false");
  usp.set("paging", "false");
  usp.set("source", (Deno.env.get("SOURCE") === "en") ? "en" : "zh-CN");
  usp.set("target", "ko");
  usp.set("text", text);

  const resp = await fetch(Deno.env.get("PAPAGO_API_URL")!, {
    method: "post",
    headers: new Headers({
      "authorization": `PPG ${uuid}:${auth}`,
      "timestamp": timestamp,
    }),
    body: usp,
  });

  console.log(Deno.env.get("PAPAGO_API_URL")!, resp.status);

  return await resp.json();
}

async function makeMessage(txts: string[]) {
  const output = [];

  for (const txt of txts) {
    const pnins = pinyin(txt).flat().join(" ");
    const translateResult = await translate(txt);
    let dict = "";

    for (const item of (translateResult.dict?.items) ?? []) {
      dict += `## ${item.entry.replace(/<.*?>/g, "")} ${(Deno.env.get("SOURCE")! === "ch") ? pinyin(item.entry.replace(/<.*?>/g, "")).flat().join(" ") : ""}\n`;

      for (const pos of item.pos) {
        for (const meaning of pos.meanings) {
          dict += `1. ${meaning.meaning.replace(/<.*?>/g, "")}\n`
        }
      }

      dict += "\n";
    }

    output.push([txt, pnins, translateResult.translatedText].join("\n") + "\n\n" + dict);
  }


  return output;
}

async function* infinity() {
  while (true) {
    yield;

    await new Promise((ok) => {
      setTimeout(ok, 0);
    });
  }
}

function isSourceLanguage(text: string) {
  return (Deno.env.get("SOURCE")! === "en") ? (/[a-zA-Z]/g).test(text): (/[\u4e00-\u9fa5]/g).test(text);
}

async function pollTgMessage() {
  let offset = 0;

  for await (const _ of infinity()) {
    try {
      const resp = await fetch(
        `https://api.telegram.org/bot${Deno.env.get(
          "BOT_TOKEN",
        )!}/getUpdates?offset=${offset}`,
        {
          method: "get",
        },
      );

      console.log(`https://api.telegram.org/bot${Deno.env.get(
        "BOT_TOKEN",
      )!}/getUpdates?offset=${offset}`, resp.status);

      const update = await resp.json();

      for (const result of update.result) {
        offset = result.update_id + 1;

        if (Deno.env.get("CHAT_ID")!) {
          if ("photo" in result.message) {
            const photo = result.message.photo.reverse()[0];

            let resp = await fetch(
              `https://api.telegram.org/bot${Deno.env.get(
                "BOT_TOKEN",
              )!}/getFile`,
              {
                method: "post",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({
                  file_id: photo.file_id,
                }),
              },
            );
  
            console.log(`https://api.telegram.org/bot${Deno.env.get("BOT_TOKEN")!}/getFile`, resp.status);

            const file = (await resp.json())["result"];

            resp = await fetch(
              `https://api.telegram.org/file/bot${Deno.env.get(
                "BOT_TOKEN",
              )!}/${file.file_path}`,
              {
                method: "get",
              },
            );

            const fd = new FormData();

            fd.set(
              "file",
              new File([await resp.bytes()], "output.jpg"),
            );

            resp = await fetch(
              `${Deno.env.get("INFER_API_URL")!}/infer/${Deno.env.get("SOURCE")!}`,
              {
                method: "post",
                body: fd,
              },
            );

            console.log(`${Deno.env.get("INFER_API_URL")!}/infer/${Deno.env.get("SOURCE")!}`, resp.status);

            const detectionMap = new Map();
            const detections = await resp.json();

            for (const detection of detections[0]) {
              const [leftUpper, , , leftBottom] = detection[0];
              const [txt] = detection[1];

              if (isSourceLanguage(txt)) {
                let near = null;

                for (const [candidateDetection] of detectionMap) {
                  const [x1, y1] = candidateDetection;
                  const [x2, y2] = leftUpper;

                  if ((x2 - x1) ** 2 <= parseInt(Deno.env.get("X_DELTA")!) && (y2 - y1) ** 2 <= parseInt(Deno.env.get("Y_DELTA")!)) {
                    near = candidateDetection;

                    break;
                  }
                }

                if (near) {
                  const oldTxt = detectionMap.get(near);

                  detectionMap.set(leftBottom, oldTxt + txt.trim());
                  detectionMap.delete(near);
                } else {
                  detectionMap.set(leftBottom, txt.trim());
                }
              }
            }

            let messages = ["no detections."];

            if (detectionMap.size > 0) {
              const txts = [];

              for (const [, txt] of detectionMap) {
                txts.push(txt);
              }

              messages = await makeMessage(txts);
            }

            for (const message of messages) {
              resp = await fetch(
                `https://api.telegram.org/bot${Deno.env.get(
                  "BOT_TOKEN",
                )!}/sendMessage`,
                {
                  method: "post",
                  headers: { "content-type": "application/json" },
                  body: JSON.stringify({
                    chat_id: Deno.env.get("CHAT_ID")!,
                    text: message,
                  }),
                },
              );

              console.log(`https://api.telegram.org/bot${Deno.env.get(
                "BOT_TOKEN",
              )!}/sendMessage`, resp.status, await resp.json());
            }
          } else {
            const oldVer = papagoVersion;
            const newVer = await renewPapagoVersion();

            papagoVersion = newVer;

            const resp = await fetch(
              `https://api.telegram.org/bot${Deno.env.get(
                "BOT_TOKEN",
              )!}/sendMessage`,
              {
                method: "post",
                headers: { "content-type": "application/json" },
                body: JSON.stringify({
                  chat_id: result.message.chat.id,
                  text: `이전 버전: ${oldVer}\n새로 찾은 버전: ${newVer}`,
                }),
              },
            );

            console.log(`https://api.telegram.org/bot${Deno.env.get(
              "BOT_TOKEN",
            )!}/sendMessage`, resp.status, await resp.json());
          }
        } else {
          const resp = await fetch(
            `https://api.telegram.org/bot${Deno.env.get(
              "BOT_TOKEN",
            )!}/sendMessage`,
            {
              method: "post",
              headers: { "content-type": "application/json" },
              body: JSON.stringify({
                chat_id: result.message.chat.id,
                text: result.message.chat.id,
              }),
            },
          );

          console.log(`https://api.telegram.org/bot${Deno.env.get(
            "BOT_TOKEN",
          )!}/sendMessage`, resp.status, await resp.json());
        }
      }

      await new Promise((ok) => {
        setTimeout(ok, 300);
      });
    } catch (e) {
      console.error(e);
    }
  }
}

(function main() {
  pollTgMessage();
})();
