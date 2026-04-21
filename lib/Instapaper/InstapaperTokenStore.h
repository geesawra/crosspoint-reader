#pragma once
#include <string>

/**
 * Loads Instapaper OAuth 1.0a credentials from an SD-card text file.
 * File format — four lines of key=value:
 *   consumer_key=xxxxxxxx
 *   consumer_secret=xxxxxxxx
 *   oauth_token=xxxxxxxx
 *   oauth_token_secret=xxxxxxxx
 *
 * User generates the file with scripts/instapaper_auth.py on a computer
 * and drops it onto the SD card. No on-device login flow.
 */
class InstapaperTokenStore {
 public:
  static InstapaperTokenStore& getInstance() { return instance; }

  InstapaperTokenStore(const InstapaperTokenStore&) = delete;
  InstapaperTokenStore& operator=(const InstapaperTokenStore&) = delete;

  // Load from /.crosspoint/instapaper_tokens.txt. Returns true if all four
  // fields are present and non-empty.
  bool loadFromFile();

  bool hasTokens() const;
  void invalidate();

  const std::string& getConsumerKey() const { return consumerKey; }
  const std::string& getConsumerSecret() const { return consumerSecret; }
  const std::string& getOauthToken() const { return oauthToken; }
  const std::string& getOauthTokenSecret() const { return oauthTokenSecret; }

 private:
  InstapaperTokenStore() = default;
  static InstapaperTokenStore instance;

  std::string consumerKey;
  std::string consumerSecret;
  std::string oauthToken;
  std::string oauthTokenSecret;
};

#define INSTAPAPER_TOKENS InstapaperTokenStore::getInstance()
