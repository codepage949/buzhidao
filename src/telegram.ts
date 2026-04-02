export type TelegramUpdate = {
  update_id: number;
  callback_query?: {
    id: string;
    data: string;
  };
  message?: {
    text?: string;
    chat: {
      id: number;
    };
  };
};

type TelegramConfig = {
  apiBaseUrl: string;
  botToken: string;
};

export class TelegramClient {
  constructor(private readonly config: TelegramConfig) {}

  async getUpdates(offset: number): Promise<TelegramUpdate[]> {
    const response = await fetch(this.buildUrl(`getUpdates?offset=${offset}`));
    console.log(
      `${this.config.apiBaseUrl}/bot.../getUpdates?offset=${offset}`,
      response.status,
    );

    const payload = await response.json();
    return payload.result ?? [];
  }

  async sendMessage(
    body: Record<string, unknown>,
  ): Promise<Response> {
    return await this.post("sendMessage", body);
  }

  async answerCallbackQuery(callbackQueryId: string): Promise<Response> {
    return await this.post("answerCallbackQuery", {
      callback_query_id: callbackQueryId,
    });
  }

  private buildUrl(path: string): string {
    return `${this.config.apiBaseUrl}/bot${this.config.botToken}/${path}`;
  }

  private async post(
    path: string,
    body: Record<string, unknown>,
  ): Promise<Response> {
    const response = await fetch(this.buildUrl(path), {
      method: "post",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(body),
    });

    console.log(
      `${this.config.apiBaseUrl}/bot.../${path}`,
      response.status,
      await response.clone().json(),
    );

    return response;
  }
}
