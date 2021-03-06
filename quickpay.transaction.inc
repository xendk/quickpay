<?php
/**
 * @file
 * Quickpay and QuickpayTransaction classes.
 */

class QuickpayException extends Exception {}

/**
 * Quickpay setup.
 *
 * Thes class abstracts a specific setup. Each
 * QuickpayTransaction/QuickpaySubscription is associated with a Quickpay class,
 * containing the configuration.
 */

class Quickpay {
  protected static $currencyInfo = array();
  protected $testMode = FALSE;
  // protected $debug = FALSE;
  protected $merchant;
  protected $privateKey;
  protected $agreementId;
  protected $agreementApiKey;
  protected $orderPrefix = '';
  protected $acceptedCards = array('creditcard');
  // With a 'use user language' default option.
  protected $language = LANGUAGE_NONE;

  // Send log to debug() rather than watchdog().
  protected $logToDebug = FALSE;
  /**
   * Constructor. Takes a settings array.
   */
  public function __construct($options) {
    // Check options, throw exception if problems.
    if (empty($options['merchant']) || !preg_match('/^\d+$/', $options['merchant'])) {
      throw new QuickpayException(t('Invalid merchant number'));
    }
    $this->merchant = $options['merchant'];

    if (empty($options['private_key'])) {
      throw new QuickpayException(t('Empty private_key.'));
    }
    $this->privateKey = $options['private_key'];

    if (empty($options['agreement_id'])) {
      throw new QuickpayException(t('Empty agreement_id.'));
    }
    $this->agreementId = $options['agreement_id'];

    if (empty($options['agreement_api_key'])) {
      throw new QuickpayException(t('Empty agreement_api_key.'));
    }
    $this->agreementApiKey = $options['agreement_api_key'];

    if (isset($options['test_mode'])) {
      $this->testMode = (bool) $options['test_mode'];
    }
    // if (isset($options['debug'])) {
    //   $this->debug = (bool) $options['debug'];
    // }
    if (isset($options['order_prefix'])) {
      $this->orderPrefix = $options['order_prefix'];
    }
    // @todo validate this to some extend.
    if (isset($options['accepted_cards'])) {
      if (is_array($options['accepted_cards'])) {
        $this->acceptedCards = $options['accepted_cards'];
      }
      else {
        $this->acceptedCards = array($options['accepted_cards']);
      }
    }
    if (isset($options['language'])) {
      $this->language = $options['language'];
    }
    if (isset($options['log_to_debug'])) {
      $this->logToDebug = (bool) $options['log_to_debug'];
    }
  }

  /**
   * Get a transaction associated with this Quickpay instance.
   */
  public function transaction($txnid) {
    return new QuickpayTransaction($this, $txnid);
  }

  /**
   * Load a transaction from HTTP POST.
   *
   * @return QuickpayTransaction|null
   *   Either a QuickpayTransaction object, or NULL if there was a problem.
   */
  public function transactionCallback() {
    if (!isset($_SERVER['HTTP_QUICKPAY_CHECKSUM_SHA256'])) {
      watchdog('quickpay', 'No checksum in callback. Ignoring.',
               array(), WATCHDOG_ERROR);
      return NULL;
    }

    // Can't use $_POST with JSON request.
    $request_body = file_get_contents("php://input");
    $request = json_decode($request_body);

    // Check that it validates.
    $checksum_calculated = $this->getCallbackChecksum($request_body);
    $checksum_requested = $_SERVER['HTTP_QUICKPAY_CHECKSUM_SHA256'];

    if ($checksum_calculated !== $checksum_requested) {
      watchdog('quickpay', 'Transaction callback checksum didn\'t verify. Checksum calculated: @hash Data: @data',
        array(
          '@data' => print_r($request, TRUE),
          '@hash' => $checksum_calculated,
        ), WATCHDOG_ERROR);
      return NULL;
    }

    $txn = new QuickpayTransaction($this);
    $txn->loadResponse($request);
    return $txn;
  }

  /**
   * Calculates the hash for the callback request.
   *
   * @param string $callback_data
   *   The data received by the callback page.
   *
   * @return string
   *   The checksum.
   */
  protected function getCallbackChecksum($callback_data) {
    return hash_hmac("sha256", $callback_data, $this->privateKey);
  }

  /**
   * Returns a form for redirecting user to Quickpay for payment.
   *
   * @param string $order_id
   *   The order ID. This will typically be an ID from the system that is
   *   managing payments, e.g. a Commerce order ID.
   * @param string|int|float $amount
   *   The amount.
   * @param string $currency
   *   The ISO 4217 currency code.
   * @param string $module
   *   The name of the module managing this payment.
   * @param array $options
   *   An array of options. Properties include:
   *   - 'continue_url': The absolute URL that Quickpay should redirect the user
   *     to when the payment is complete.
   *   - 'cancel_url': The absolute URL that Quickpay should redirect the user
   *     to when the payment is cancelled.
   *   - 'type': The type of authorization, either 'payment', for regular
   *     payments, or 'subscription' for subscription signups. Optional,
   *     defaults to 'payment'.
   *   - 'description': Description of subscription, required for msgtype
   *     'subscribe' .
   *   - 'autocapture': Capture the payment immediately. Must only be used if
   *     the order is fulfilled immediately, for example if paying for access
   *     to some content that can then be viewed strait away.
   *   - 'autofee': Calculate and add the fee for the acquirer to the amount.
   *     This will increase the amount charged.
   *
   * @return array
   *   A from render array. The form will submit directly to Quickpay.
   */
  public function form($order_id, $amount, $currency, $module, $options) {
    $options += array(
      'autocapture' => FALSE,
      'type' => 'payment',
      'description' => NULL,
      'continue_url' => NULL,
      'cancel_url' => NULL,
    );

    if (empty($options['continue_url'])) {
      throw new QuickpayException(t("Missing required continue_url."));
    }

    if (empty($options['cancel_url'])) {
      throw new QuickpayException(t("Missing required cancel_url."));
    }

    if (!in_array($options['type'], array('payment', 'subscription'))) {
      // FIXME: better error handling.
      throw new QuickpayException(t("Bad message type @type, must be 'payment' or 'subscription'.", array('@type' => $options['type'])));
    }

    if ($options['type'] == 'subscription') {
      if (empty($options['description'])) {
        throw new QuickpayException(t("Missing required subscription description."));
      }
      if ($options['autocapture']) {
        throw new QuickpayException(t("Autocapture not possible for subscriptions."));
      }
    }

    if (!module_hook($module, 'quickpay_factory')) {
      throw new QuickpayException(t("%module doesn't implement hook_quickpay_factory.", array('%module' => $module)));
    }

    if (!module_hook($module, 'quickpay_callback')) {
      throw new QuickpayException(t("%module doesn't implement hook_quickpay_callback.", array('%module' => $module)));
    }

    $data = array();
    // Required variables.
    $data['version'] = QUICKPAY_VERSION;
    $data['merchant_id'] = $this->merchant;
    $data['agreement_id'] = $this->agreementId;
    $data['order_id'] = $this->orderNumber($order_id);

    $currency_info = Quickpay::currencyInfo($currency);
    $data['amount'] = Quickpay::wireAmount($amount, $currency_info);
    $data['currency'] = $currency_info['code'];
    $data['continueurl'] = $options['continue_url'];
    $data['cancelurl'] = $options['cancel_url'];
    // End of required variables.
    $data['callbackurl'] = url(quickpay_hash_path('quickpay/' . $order_id . '/' . $module), array('absolute' => TRUE));

    if ($options['type'] !== 'payment') {
      $data['type'] = $options['type'];
    }

    // Use the user's current language.
    if ($this->language == LANGUAGE_NONE) {
      global $language;
      $data['language'] = self::getCountryCode($language->language);
    }
    else {
      $data['language'] = $this->language;
    }

    if ($this->acceptedCards) {
      $data['payment_methods'] = implode(',', $this->acceptedCards);
    }

    $data['autocapture'] = ($options['autocapture']) ? '1' : '0';

    // Build the checksum.
    $data['checksum'] = $this->getChecksum($data);

    $form['#method'] = 'POST';
    $form['#action'] = 'https://payment.quickpay.net';

    foreach ($data as $name => $value) {
      $form[$name] = array('#type' => 'hidden', '#value' => $value);
    }

    $button_id = drupal_html_id('quicpay-submit-button');
    $form['submit'] = array(
      '#type' => 'submit',
      '#value' => t('Continue to QuickPay'),
      '#id' => $button_id,
    );

    return $form;
  }

  /**
   * Return Quickpay client.
   *
   * @return Quickpay\Quickpay
   *   The client.
   */
  public function client() {
    module_load_include('php', 'quickpay', 'lib/QuickPay/QuickPay');
    return new Quickpay\Quickpay(':' . $this->agreementApiKey);
  }

  /**
   * Calculate the hash for the request.
   *
   * @param array $data
   *   The data to POST to Quickpay.
   *
   * @return string
   *   The checksum.
   *
   * @see http://tech.quickpay.net/payments/hosted/#checksum
   */
  protected function getChecksum(array $data) {
    $api_key = $this->agreementApiKey;
    $flattened_params = $this->flattenParams($data);
    ksort($flattened_params);
    $base = implode(" ", $flattened_params);
    return hash_hmac("sha256", $base, $api_key);
  }

  /**
   * Flatten request parameter array.
   */
  protected function flattenParams($obj, $result = array(), $path = array()) {
    if (is_array($obj)) {
      foreach ($obj as $k => $v) {
        $result = array_merge($result, $this->flattenParams($v, $result, array_merge($path, array($k))));
      }
    }
    else {
      $result[implode("", array_map(function($param) {
        return "[{$param}]";
      }, $path))] = $obj;
    }

    return $result;
  }

  /**
   * Authorize a payment through the API. Only for test use.
   *
   * In order to handle credit card information, you'll either need a full PCI
   * certification which can cost 15000EUR or more, or have a signed
   * dispensation from Nets (which you're very unlikely to get), so this
   * function is prohibited for most sites. It is rather handy for unit
   * testing the API though.
   *
   * Just to be clear: using this function without PCI certification or
   * dispensation could mean that Nets shuts down your agreement without
   * notice.
   *
   * However, if you *are* allowed to use this function, please contact the
   * maintainer, so he knows it's in fact being used.
   */
  public function authorize($cardnumber, $expiration, $cvd, $order_id, $amount, $currency, $options = array()) {
    $currency_info = Quickpay::currencyInfo($currency);

    // Create payment.
    $data = array(
      'order_id' => $this->orderNumber($order_id),
      'currency' => $currency_info['code'],
    );
    $payment_res = $this->client()->request->post('/payments', $data);

    if (!$payment_res->is_success()) {
      $this->logError('Error creating payment.', $payment_res);
      throw new QuickpayException('Error creating payment.');
    }

    $payment = $payment_res->as_object();
    // Authorise the payment.
    $data = array(
      'amount' => Quickpay::wireAmount($amount, $currency_info),
      'card[number]' => $cardnumber,
      'card[expiration]' => $expiration,
      'card[cvd]' => $cvd,
      'auto_capture' => isset($options['autocapture']) && $options['autocapture'],
      'test_mode' => $this->testMode,
      // Apparently Quickpay doesn't figure out the acquirer itself when using
      // the API anymore. As this is only for testing, we hardcore it.
      'acquirer' => 'clearhaus',
    );

    $authorize_res = $this->client()->request->post("/payments/{$payment->id}/authorize?synchronized", $data);

    if (!$authorize_res->is_success()) {
      $this->logError('Error authorizing payment.', $authorize_res);
      throw new QuickpayException('Error authorizing payment.');
    }

    $transaction = new QuickpayTransaction($this);
    $transaction->loadResponse($authorize_res->as_object());
    return $transaction;
  }

  /**
   * Subscribe test function. See authorize().
   */
  public function subscribe($cardnumber, $expiration, $cvd, $order_id, $description, $amount, $currency) {
    // Currently usupported, the API fails for some reason.
    throw new QuickpayException("Not implemented.");
    $currency_info = Quickpay::currencyInfo($currency);

    // Create subscription.
    $data = array(
      'order_id' => $this->orderNumber($order_id),
      'currency' => $currency_info['code'],
      'description' => $description,
    );
    $subscription_res = $this->client()->request->post('/subscriptions', $data);

    if (!$subscription_res->is_success()) {
      $this->logError('Error creating subscription.', $subscription_res);
      throw new QuickpayException('Error creating subscription.');
    }

    $subscription = $subscription_res->as_object();
    // Authorise the subscription.
    $data = array(
      'amount' => Quickpay::wireAmount($amount, $currency_info),
      'card[number]' => $cardnumber,
      'card[expiration]' => $expiration,
      'card[cvd]' => $cvd,
      'test_mode' => $this->testMode,
      // Apparently Quickpay doesn't figure out the acquirer itself when using
      // the API anymore. As this is only for testing, we hardcore it.
      'acquirer' => 'clearhaus',
    );

    $authorize_res = $this->client()->request->post("/subscriptions/{$subscription->id}/authorize?synchronized", $data);

    if (!$authorize_res->is_success()) {
      drush_print_r($authorize_res->as_object());
      $this->logError('Error authorizing subscription.', $authorize_res);
      throw new QuickpayException('Error authorizing subscription.');
    }

    $transaction = new QuickpaySubscription($this);
    $transaction->loadResponse($authorize_res->as_object());
    return $transaction;
  }

  /**
   * Ensure that ordernumber is properly formatted.
   */
  public function orderNumber($order_id) {
    $ordernumber = $this->orderPrefix . $order_id;
    // Ensure that Order number is at least 4 characters. Else Quickpay will
    // reject the request.
    if (strlen($ordernumber) < 4) {
      $ordernumber = $this->orderPrefix . substr('0000' . $order_id,
                     -4 + strlen($this->orderPrefix));
    }
    return $ordernumber;
  }

  /**
   * Returns an array of languages supported by Quickpay.
   *
   * @return array
   *   Array of code => name.
   */
  protected static function getLanguages() {
    return array(
      'da' => t('Danish'),
      'de' => t('German'),
      'en' => t('English'),
      'fo' => t('Faeroese'),
      'fr' => t('French'),
      'gl' => t('Greenlandish'),
      'it' => t('Italian'),
      'no' => t('Norwegian'),
      'nl' => t('Dutch'),
      'pl' => t('Polish'),
      'se' => t('Swedish'),
    );
  }

  /**
   * Return the associated country code of a language code.
   *
   * @param string $langcode
   *   ISO Language code as used by Drupal.
   *
   * @return string
   *   ISO country code.
   */
  protected static function getCountryCode($langcode) {
    $langcodes = array(
      'da' => 'da',
      'de' => 'de',
      'en' => 'en',
      'fo' => 'fo',
      'fr' => 'fr',
      'kl' => 'gl',
      'it' => 'it',
      'nb' => 'no',
      'nn' => 'no',
      'nl' => 'nl',
      'pl' => 'pl',
      'sv' => 'se',
    );

    return isset($langcodes[$langcode]) ? $langcodes[$langcode] : 'en';
  }

  /**
   * Returns the settings form.
   *
   * @param array $settings
   *   Previously saved setting.
   *
   * @return array
   *   Form array for the settings.
   */
  public static function settingsForm($settings = array()) {
    $settings += array(
      'merchant' => '',
      'private_key' => '',
      'agreement_id' => '',
      'agreement_api_key' => '',
      'order_prefix' => '',
      'language' => LANGUAGE_NONE,
      'accepted_cards' => array('creditcard'),
      'test_mode' => FALSE,
      // 'debug' => FALSE,
    );

    $form['merchant'] = array(
      '#type' => 'textfield',
      '#title' => t('Merchant ID'),
      '#description' => t('The Merchant ID as shown in the QuickPay admin.'),
      '#default_value' => $settings['merchant'],
      '#required' => TRUE,
    );

    $form['private_key'] = array(
      '#type' => 'textfield',
      '#title' => t('Private key'),
      '#description' => t('Your private key.'),
      '#default_value' => $settings['private_key'],
      '#required' => TRUE,
    );

    $form['agreement_id'] = array(
      '#type' => 'textfield',
      '#title' => t('Agreement ID'),
      '#description' => t('This is the Payment Window Agreement ID.'),
      '#default_value' => $settings['agreement_id'],
      '#required' => TRUE,
    );

    $form['agreement_api_key'] = array(
      '#type' => 'textfield',
      '#title' => t('API key'),
      '#description' => t('This is the Payment Window API key.'),
      '#default_value' => $settings['agreement_api_key'],
      '#required' => TRUE,
    );

    $form['order_prefix'] = array(
      '#type' => 'textfield',
      '#title' => t('Order id prefix'),
      '#description' => t('Prefix for order ids. Order ids must be uniqe when sent to QuickPay, use this to resolve clashes.'),
      '#default_value' => $settings['order_prefix'],
      '#element_validate' => array('quickpay_order_prefix_validate'),
    );

    $languages = self::getLanguages() + array(LANGUAGE_NONE => t('Language of the user'));

    $form['language'] = array(
      '#type' => 'select',
      '#title' => t('Language'),
      '#description' => t('The language for the credit card form.'),
      '#options' => $languages,
      '#default_value' => $settings['language'],
    );

    $form['accepted_cards'] = array(
      '#type' => 'quickpay_payment_method',
      '#default_value' => $settings['accepted_cards'],
    );

    $form['test_mode'] = array(
      '#type' => 'checkbox',
      '#title' => t('Test mode'),
      '#description' => t('When active, transactions will be run in test mode, even if the QuickPay account is in production mode. Order ids will get a T appended.'),
      '#default_value' => $settings['test_mode'],
    );

    // $form['debug'] = array(
    //   '#type' => 'checkbox',
    //   '#title' => t('Debug log'),
    //   '#description' => t('Log all request and responses to QuickPay in watchdog.'),
    //   '#default_value' => $settings['debug'],
    // );

    return $form;
  }

  /**
   * Get information about an currency.
   *
   * @param string $code
   *   The ISO 4217 currency code.
   *
   * @return NULL|array
   *   An array with the keys 'code' and 'multiplier', or null if not found.
   */
  public static function currencyInfo($code) {
    // @todo if commerce module is enabled, use its list.
    if (!array_key_exists($code, Quickpay::$currencyInfo)) {
      // Use a basic set.
      $base_currencies = array(
        'DKK' => array('code' => 'DKK', 'multiplier' => 100),
        'USD' => array('code' => 'USD', 'multiplier' => 100),
        'EUR' => array('code' => 'EUR', 'multiplier' => 100),
        'GBP' => array('code' => 'GBP', 'multiplier' => 100),
        'SEK' => array('code' => 'SEK', 'multiplier' => 100),
        'NOK' => array('code' => 'NOK', 'multiplier' => 100),
        'ISK' => array('code' => 'ISK', 'multiplier' => 100),
      );

      Quickpay::$currencyInfo += $base_currencies;
      // If still not found, throw an exception.
      if (!array_key_exists($code, Quickpay::$currencyInfo)) {
        throw new QuickpayException(t('Unknown currency code %currency', array('%currency' => $code)));
      }
    }
    return Quickpay::$currencyInfo[$code];
  }

  /**
   * Returns the amount adjusted by the multiplier for the currency.
   *
   * @param decimal $amount
   *   The amount.
   * @param array|string $currency_info
   *   An currencyInfo() array, or a currency code.
   */
  public static function wireAmount($amount, $currency_info) {
    if (!is_array($currency_info)) {
      $currency_info = Quickpay::currencyInfo($currency_info);
    }
    return (function_exists('bcmul') ?
        bcmul($amount, $currency_info['multiplier']) :
        $amount * $currency_info['multiplier']);
  }

  /**
   * Reverses wireAmount().
   *
   * @param int $amount
   *   The amount.
   * @param array|string $currency_info
   *   An currencyInfo() array, or a currency code.
   */
  public static function unwireAmount($amount, $currency_info) {
    if (!is_array($currency_info)) {
      $currency_info = Quickpay::currencyInfo($currency_info);
    }
    return (function_exists('bcdiv') ?
        bcdiv($amount, $currency_info['multiplier'], log10($currency_info['multiplier'])) :
        $amount / $currency_info['multiplier']);

  }

  /**
   * Log a client error.
   */
  public function logError($message, $response) {
    $error = $response->as_object();
    if (!empty($error->message)) {
      $message .= "\n" . $error->message;
    }
    if (!empty($error->errors)) {
      foreach ($error->errors as $key => $val) {
        $message .= "\n" . $key . ': ' . implode(', ', $val);
      }
    }

    if ($this->logToDebug) {
      debug($message);
      // Bad errors doesn't return an object, but a string.
      debug($response);
    }
    else {
      //watchdog('quickpay', '@message', array('@message' => $message), WATCHDOG_ERROR);
    }
  }
}

/**
 * Base class for transactions and subscriptions.
 */
class QuickpayTransactionBase {
  protected $qp;
  protected $data;
  protected $loaded = FALSE;

  /**
   * Create a transaction object.
   */
  public function __construct(Quickpay $qp, $txnid = NULL) {
    $this->qp = $qp;
    $this->data = array();
    if (preg_match('/^a:/', $txnid)) {
      list($this->data['id'], $this->data['currency']) = unserialize($txnid);
    }
    else {
      $this->data['id'] = $txnid;
    }
  }

  /**
   * Magic method.
   */
  public function __get($field) {
    // Krumo sets odd properties on object, let it.
    if (preg_match('/krumo/', $field)) {
      return $this->{$field};
    }

    if (!$this->loaded) {
      $this->status();
    }

    if (in_array($field, array('currency', 'amount'))) {
      return $this->{$field}();
    }

    // Legacy. Should have made an abstracted message property/method earlier,
    // but that's too late.
    if ($field == 'qpstatmsg') {
      return $this->lastMessage();
    }

    // More legacy.
    if ($field == 'transaction') {
      $field = 'id';
    }

    if (array_key_exists($field, $this->data)) {
      return $this->data[$field];
    }

    throw new QuickpayException(t("Unknown property @name", array('@name' => $field)));
  }

  /**
   * Magic method.
   */
  public function __isset($field) {
    if ($field != 'id' && !$this->loaded) {
      $this->status();
    }
    if (isset($this->data[$field])) {
      return $this->data[$field];
    }
  }

  /**
   * Get last transaction operation.
   */
  protected function getLastOperation() {
    if (!$this->loaded) {
      $this->status();
    }
    if (isset($this->data['operations'])) {
      $operations = $this->data['operations'];
      return array_pop($operations);
    }

    return NULL;
  }

  /**
   * Get last operation message.
   */
  public function lastMessage() {
    if ($operation = $this->getLastOperation()) {
      return $operation->qp_status_msg;
    }

    return '';
  }

  /**
   * Get last operation state.
   */
  public function lastState() {
    if ($operation = $this->getLastOperation()) {
      return $operation->type;
    }

    return '';
  }

  /**
   * Returns the currency for this transaction.
   */
  public function currency() {
    if (!$this->loaded) {
      $this->status();
    }
    // @todo call status command if not set.
    if ($this->data['currency']) {
      return $this->data['currency'];
    }
    throw new QuickpayException(t('No currency defined for transaction @txn_id', array('@txn_id' => $this->data['id'])));
  }

  /**
   * Get the wire amount.
   *
   * Returns the amount adjusted by the multiplier for the currency of this
   * transaction.
   */
  public function wireAmount($amount = NULL) {
    if (!$amount) {
      return $this->data['amount'];
    }
    return Quickpay::wireAmount($amount, $this->currency());
  }

  /**
   * Load a request response.
   */
  public function loadResponse($response) {
    if (is_object($response)) {
      $response = (array) $response;
    }
    if (!is_array($response)) {
      throw new QuickpayException(t('Transaction could not be loaded from response: !response', array(
            '!response', print_r($response, TRUE),
          )));
    }

    $this->data = $response;
    $this->loaded = TRUE;
  }

  /**
   * Retung transaction storage sting.
   *
   * Encode the minimal needed information about this transaction, for later
   * loading. This is what modules using Quickpay should use as a reference.
   */
  public function store() {
    if ($this->data['currency']) {
      return serialize(array($this->data['id'], $this->data['currency']));
    }
    return $this->data['id'];
  }


  /**
   * Whether this transaction was a success.
   */
  public function success() {
    if ($operation = $this->getLastOperation()) {
      if ($operation->qp_status_code == '20000') {
        return TRUE;
      }
    }

    return FALSE;
  }
}

/**
 * Abstracts a transaction.
 */
class QuickpayTransaction extends QuickpayTransactionBase {
  /**
   * Returns the amount for this transaction.
   */
  public function amount() {
    if (!$this->loaded) {
      $this->status();
    }
    if (isset($this->data['amount'])) {
      return Quickpay::unwireAmount($this->data['amount'], $this->currency());
    }
    throw new QuickpayException(t('No amount for transaction @txn_id', array('@txn_id', $this->data['id'])));
  }

  /**
   * Returns the authorized amount for this transaction.
   */
  public function authorizedAmount() {
    if (!$this->loaded) {
      $this->status();
    }
    if (isset($this->data['operations'])) {
      foreach ($this->data['operations'] as $operation) {
        if ($operation->type == 'authorize') {
          return Quickpay::unwireAmount($operation->amount, $this->currency());
        }
      }
    }

    return NULL;
  }

  /**
   * Return the balance for this transaction.
   *
   * This is (for histotical reasons) how much is still left to be captured of
   * the authorized amount.
   */
  public function balance() {
    // The v10 API keeps track of the balance, but as how much has been
    // captured. So we find the authorization amount and subtracts the balance
    // from that.
    if (!$this->loaded) {
      $this->status();
    }
    if (isset($this->data['operations'])) {
      foreach ($this->data['operations'] as $operation) {
        if ($operation->type == 'authorize') {
          $balance = $operation->amount;
          break;
        }
      }
      if ($balance) {
        return Quickpay::unwireAmount($balance - $this->data['balance'], $this->currency());
      }
    }
  }

  /**
   * Capture on an authorised transaction.
   */
  public function capture($amount) {
    // Capture payment.
    $data = array(
      'amount' => $this->wireAmount($amount),
    );
    $capture_res = $this->qp->client()->request->post("/payments/{$this->data['id']}/capture?synchronized", $data);

    if (!$capture_res->is_success()) {
      $this->qp->logError('Error capturing payment.', $capture_res);
      throw new QuickpayException('Error capturing payment.');
    }

    $this->loadResponse($capture_res->as_object());
  }

  /**
   * Update this transaction with status information from Quickpay.
   */
  public function status() {
    $payment_res = $this->qp->client()->request->get("/payments/{$this->data['id']}");

    if (!$payment_res->is_success()) {
      $this->qp->logError('Error fetching payment.', $payment_res);
      throw new QuickpayException('Error fetching payment.');
    }

    $this->loadResponse($payment_res->as_object());
  }

  /**
   * Renew an authorisation.
   */
  public function renew() {
    // Currently usupported, the API fails for some reason.
    throw new QuickpayException("Not implemented.");

    $renew_res = $this->qp->client()->request->post("/payments/{$this->data['id']}/renew?synchronized");

    if (!$renew_res->is_success()) {
      $this->qp->logError('Error renewing payment.', $renew_res);
      throw new QuickpayException('Error renewing payment.');
    }

    $this->loadResponse($renew_res->as_object());
  }

  /**
   * Cancel an authorised transaction.
   */
  public function cancel() {
    $cancel_res = $this->qp->client()->request->post("/payments/{$this->data['id']}/cancel?synchronized");

    if (!$cancel_res->is_success()) {
      $this->qp->logError('Error cancelling payment.', $cancel_res);
      throw new QuickpayException('Error cancelling payment.');
    }

    $this->loadResponse($cancel_res->as_object());
  }

  /**
   * Refund previously captured money.
   */
  public function refund($amount) {
    $data = array(
      'amount' => $this->wireAmount($amount),
    );
    $refund_res = $this->qp->client()->request->post("/payments/{$this->data['id']}/refund?synchronized", $data);

    if (!$refund_res->is_success()) {
      $this->qp->logError('Error refunding payment.', $refund_res);
      throw new QuickpayException('Error refunding payment.');
    }

    $this->loadResponse($refund_res->as_object());
  }
}

/**
 * Represents a scubscription.
 */
class QuickpaySubscription extends QuickpayTransactionBase {
  /**
   * Update this subscription with status information from Quickpay.
   */
  public function status() {
    $subscription_res = $this->qp->client()->request->get("/subscriptions/{$this->data['id']}");

    if (!$subscription_res->is_success()) {
      $this->qp->logError('Error fetching subscription.', $subscription_res);
      throw new QuickpayException('Error fetching subscription.');
    }

    $this->loadResponse($subscription_res->as_object());
  }

  /**
   * Create a new payment on a subscription transaction.
   */
  public function recurring($order_id, $amount, $options = array()) {
    $data = array(
      'order_id' => $order_id,
      'amount' => $this->wireAmount($amount),
      'auto_capture' => isset($options['autocapture']) && $options['autocapture'],
    );
    $recurring_res = $this->qp->client()->post("/subscriptions/{$this->data['id']}/recurring", $data);

    if (!$recurring_res->is_success()) {
      $this->qp->logError('Error authorizing payment.', $recurring_res);
      throw new QuickpayException('Error authorizing payment.');
    }

    $transaction = new QuickpayTransaction($this);
    $transaction->loadResponse($recurring_res->as_object());
    return $transaction;
  }
}
