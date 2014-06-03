#include <fc/crypto/openssl.hpp>

#include <fc/filesystem.hpp>

#include <boost/filesystem/path.hpp>

#include <cstdlib>
#include <string>

namespace  fc 
{
    struct openssl_scope
    {
      static path _configurationFilePath;
       openssl_scope()
       {
          ERR_load_crypto_strings(); 
          OpenSSL_add_all_algorithms();

          const boost::filesystem::path& boostPath = _configurationFilePath;
          if(boostPath.empty() == false)
            {
            std::string varSetting("OPENSSL_CONF=");
            varSetting += _configurationFilePath.to_native_ansi_path();
            putenv(varSetting.c_str());
            }

          OPENSSL_config(nullptr);
       }

       ~openssl_scope()
       {
          EVP_cleanup();
          ERR_free_strings();
       }
    };

    path openssl_scope::_configurationFilePath;

    void store_configuration_path(const path& filePath)
      {
      openssl_scope::_configurationFilePath = filePath;
      }
   
    int init_openssl()
    {
        static openssl_scope ossl;
        return 0;
    }
}
